#!/usr/bin/env python3
"""Convert user-owned Dark Messiah viewmodel clips for Morrowind's first-person arms.

Dark Messiah adds the stored reference CameraAdd attachment to the body MMeyes eye.
With a body-camera contract, that eye follows the full-body scale; arm lengths retain
their own first-person scale. Held-item framing uses that same physical eye.
Copying joint rotations onto Morrowind's longer arms breaks that framing, so this converter
copies *positions* instead, scaled by the arm-length ratio:

* each clavicle is translated so the shoulder joint lands where DM puts it relative to the eye;
* upper arm/forearm solve two-bone IK to DM's grip, following its animated elbow hinge;
* the hand orientation maps DM's held-weapon frame (RightHand attachment, blade +X) onto
  Morrowind's Weapon Bone frame (blade +Y), so the blade points where DM's does on screen;
* the Camera node includes the absolute reference CameraAdd relative to the base eye;
* a shared root carrier preserves the body MMeyes kick recoil outside view pitch;
* a static pivot node gives the native neck controller that same base eye for view pitch.

The carrier stays neutral for other groups: their raw body head animation does not
match Dark Messiah's idle head mask.

Outputs: <output>/Animations/brutal_fp/*.kf, which OpenMW loads as
additional sources of the first-person skeleton.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import numpy as np

from retarget_kf import cross, norm, prime_quaternions
from dm_sources import archives, entries, extract, store
from inspect_mdl import load_mdl
from retarget_kf import quat_matrix, matrix_quat
from retarget_contract import CONTRACT, sides as correspondence_sides, validate as validate_correspondence, measure_grip_solver, validate_attachment

ROOT = Path(__file__).resolve().parents[2]
SOURCEIO = ROOT/'third_party/SourceIO'
FPS = 30
CAMERA_CARRIER = 'Brutal Camera Carrier'
# DM model space (x forward, y left, z up) -> Morrowind actor space (x right, y forward, z up).
BASIS = np.asarray(CONTRACT['coordinate_basis'], float)
SIDES = correspondence_sides()


def to4(R, p):
    m = np.zeros((4, 4)); m[3, 3] = 1.; m[:3, :3] = R; m[:3, 3] = p; return m


def frame(a, b):
    """Right-handed frame whose first axis is a and second axis is b made orthogonal."""
    a = a/norm(a); b = b-a*np.dot(a, b); b = b/norm(b)
    return np.column_stack([a, b, cross(a, b)])


class Viewmodel:
    untrimmed_bounce_layout = False
    rest_matrices = None
    fk_orders = None
    def __init__(self, vpk, work, skeleton, grip_kf, shield_normal=-1., shield_mesh=None, shield_keep_size=True, camera_contract=None, native_eye_height=False):
        self.vpk, self.work = vpk, work
        self.skeleton_path = skeleton
        self.models, self.sequences, self.provenance = {}, {}, []
        sys.path.insert(0, str(ROOT/'third_party/io_scene_mw/lib'))
        sys.path.insert(0, str(ROOT/'third_party'))
        from es3 import nif
        self.nif = nif
        scene = nif.NiStream(); scene.load(skeleton)
        self.nodes = {n.name: n for n in scene.objects_of_type(nif.NiNode) if n.name}
        self.rest_matrices = {}
        self.parents = {c.name: n.name for n in self.nodes.values() for c in n.children
                        if isinstance(c, nif.NiNode) and c.name}
        self.order = [n for n in self.nodes if n in self.parents or n == 'Bip01']
        self.rest = self.fk({})
        # Static finger grips from Morrowind's own first-person armed idle.
        self.grip = self.sample_kf(grip_kf, 'idle1h: start', lambda n: 'Finger' in n)
        stream = nif.NiStream(); stream.load(grip_kf)
        self.keyed = list(dict.fromkeys([e.string_data for e in stream.root.extra_datas
                      if isinstance(e, nif.NiStringExtraData) and e.string_data != 'Bip01']+['Camera', 'Shield Bone']))
        mw_arm = lambda s: (norm(self.nodes[SIDES[s]['mw'][2]].translation)
                            + norm(self.nodes[SIDES[s]['mw'][3]].translation))
        bones = self.dm_bones('first_basic')
        self.bone_correspondence = validate_correspondence(bones, self.nodes, self.parents, self.rest)
        local = {b.name: np.array(b.position) for b in bones}
        dm_arm = (norm(local['SK_R_Elbow']) + norm(local['SK_R_Forearm'])
                  + norm(local['SK_R_Wrist']) + norm(local['SK_R_Hand']))
        self.scale = mw_arm('R')/dm_arm
        reference = self.dm_world('first_animations_basic', '@anm_reference', 0.)
        self.camera_attachment = self.attachment('first_basic', 'CameraAdd')
        self.reference_camera = (reference['SK_Camera_Eyes']@self.camera_attachment)[:3, 3]
        self.camera = self.rest['Camera'][:3, 3]
        self.camera_contract = json.loads(camera_contract.read_text()) if camera_contract else None
        if self.camera_contract:
            if self.camera_contract.get('schema') != 1:
                raise ValueError('Unsupported body-camera contract schema')
            self.body_scale = float(self.camera_contract['body_scale'])
            native_pelvis = np.array(self.camera_contract['native_pelvis_reference'], float)
            source_pelvis = np.array(self.camera_contract['source_pelvis_reference'], float)
            source_eye = np.array(self.camera_contract['source_base_eye'], float)
            self.source_base_eye = source_eye
            self.base_eye = native_pelvis+self.body_scale*BASIS@(source_eye-source_pelvis)
            if not np.allclose(self.base_eye, self.camera_contract['native_base_eye'], atol=1e-5):
                raise ValueError('Body-camera contract base eye disagrees with its pelvis reference')
        self.height_calibration = None
        if native_eye_height:
            if not self.camera_contract:
                raise ValueError('Native eye calibration requires a body-camera contract')
            source_level = float((self.base_eye+self.body_scale*BASIS@self.reference_camera)[2])
            native_level = float(self.native_idle_camera(grip_kf)[2])
            delta = native_level-source_level
            self.base_eye = self.base_eye+np.array([0., 0., delta])
            self.height_calibration = dict(mode='native-idle-camera', native_level_eye=native_level,
                source_level_eye=source_level, translation=[0., 0., delta],
                source_skeleton=str(skeleton), source_animation=str(grip_kf), source_key='idle: start',
                body='unchanged', carrier='unchanged',
                notes='Original first-person Camera bone at native idle; shift eye, pitch pivot and arms together.')
        self.set_camera(reference)
        # Items play their own @idle pose at the hand attachment. Their animated root
        # differs from the MDL bind root: ignoring skinning here rotates the sword and
        # shield by 90 degrees. Measure item axes after that animation, not in raw VVD.
        self.weapon_attachment = self.attachment('first_basic', 'RightHand')
        self.weapon_idle = self.item_idle('items/weapons/sword_long/sword_long')
        self.weapon_bone = self.local('Weapon Bone')
        att, sword_idle = self.weapon_attachment, self.weapon_idle[:3, :3]
        # Both flat-face normals preserve a double-edged sword's blade axis.
        # Choose the one that maps the native knuckles/thumb towards DM's,
        # rather than turning the palm backwards around the weapon axis.
        dm_r = frame(att[:3, :3]@sword_idle@[0, -1, 0], att[:3, :3]@sword_idle@[0, 0, -1])
        weapon = self.weapon_bone[0]
        mw_r = frame(weapon@[0, 1, 0], weapon@[1, 0, 0])
        dm_l = frame(local['SK_L_Finger_B1'], local['SK_L_Thumb_1'])
        mw_l = frame(np.asarray(self.nodes['Bip01 L Finger2'].translation, float),
                     np.asarray(self.nodes['Bip01 L Finger0'].translation, float))
        self.hand_map = {'R': dm_r@mw_r.T, 'L': dm_l@mw_l.T}
        # An almost straight authored elbow has no reliable position-derived
        # bend plane: quantized points can flip its inferred normal by 180deg.
        # Its animated elbow frame still has a stable hinge direction. Calibrate
        # that direction from a bent idle and use it continuously, without a
        # threshold switch when the arm straightens.
        idle = self.dm_world('first_animations_basic', 'anm_sword_noshield_idle', 0.)
        self.dm_hinge = {}
        for side, names in SIDES.items():
            shoulder, elbow, hand = [idle[name][:3, 3] for name in names['dm'][1:]]
            normal = cross(elbow-shoulder, hand-elbow)
            self.dm_hinge[side] = idle[names['dm'][2]][:3, :3].T@(normal/norm(normal))
        # Shield set: DM Leftshield (wrist) vs Morrowind Shield Bone (hand). The actual
        # guard shield used for the in-game reference has height Z and face normal Y;
        # its own @idle maps the face to -X (not the raw mesh's +Y).
        self.shield_attachment = self.attachment('first_basic', 'Leftshield')
        shield_idle = self.item_idle('items/armors/shield_guard/shield_guard')[:3, :3]
        dm_s = frame(shield_idle@[0, 0, 1.], shield_idle@[0, shield_normal, 0])
        self.shield_bone = self.local('Shield Bone')
        mw_s = frame(np.array([0, -1., 0]), np.array([0, 0, 1.]))
        self.shield_map = dm_s@mw_s.T  # shield frame (DM item axes) -> Shield Bone axes
        self.shield_scale, self.shield_offset, self.shield_profile = 1., np.zeros(3), None
        if shield_mesh is not None:
            # Match the user's selected native shield to the reference guard shield.
            # Their mesh roots are different grip origins, and copying only axes makes
            # the larger steel shield cross the eye plane. Fit one uniform scale to
            # width/height, then align their geometric centers in attachment space.
            from SourceIO.library.models.vvd import Vvd
            from SourceIO.library.utils import MemoryBuffer
            model = 'items/armors/shield_guard/shield_guard'
            vertices = Vvd.from_buffer(MemoryBuffer(self.dm_resource(model, '.vvd').read_bytes())).lod_data[0]['vertex']
            idle = self.item_idle(model)
            source_points = vertices@idle[:3, :3].T+idle[:3, 3]
            mesh = nif.NiStream(); mesh.load(shield_mesh)
            target_points = []
            def visit(node, parent):
                transform = parent@to4(np.asarray(node.rotation)*node.scale, np.asarray(node.translation))
                if isinstance(node, nif.NiTriShape):
                    target_points.append(node.data.vertices@transform[:3, :3].T+transform[:3, 3])
                for child in getattr(node, 'children', ()):
                    if isinstance(child, nif.NiAVObject): visit(child, transform)
            for root in mesh.roots: visit(root, np.eye(4))
            target_points = np.concatenate(target_points)
            source_extent = np.ptp(source_points@dm_s, axis=0)[[0, 2]]*self.scale
            target_extent = np.ptp(target_points@mw_s, axis=0)[[0, 2]]
            fitted_scale = float(np.dot(source_extent, target_extent)/np.dot(target_extent, target_extent))
            self.shield_scale = 1. if shield_keep_size else fitted_scale
            source_center = (source_points.min(0)+source_points.max(0))/2
            target_center = (target_points.min(0)+target_points.max(0))/2
            self.shield_offset = self.scale*source_center-self.shield_map@(self.shield_scale*target_center)
            self.shield_profile = dict(native_mesh=str(shield_mesh), scale=self.shield_scale,
                size_mode='native' if shield_keep_size else 'reference', reference_fit_scale=fitted_scale,
                source_height_width=source_extent.tolist(), native_height_width=target_extent.tolist(),
                source_center=source_center.tolist(), native_center=target_center.tolist(),
                offset_item_axes=self.shield_offset.tolist())
        self.shield = False
        self.groups, self.pending = {}, []
        # Elbow hinge: Morrowind forearm rest rotation relative to the upper arm.
        self.hinge = {}
        for side in SIDES:
            R = self.local(SIDES[side]['mw'][2])[0]
            w, v = np.linalg.eig(R)
            axis = np.real(v[:, np.argmin(np.abs(w-1))])
            self.hinge[side] = axis/norm(axis)
        self.bone_correspondence['implementation'] = measure_grip_solver(self.nodes, self.rest, self.hinge)

    # --- Morrowind first-person skeleton ---------------------------------------------------
    def local(self, name, overrides=None):
        if overrides and name in overrides: return overrides[name]
        n = self.nodes[name]
        return np.asarray(n.rotation, float), np.asarray(n.translation, float)*1.0

    def rest_matrix(self, name):
        """The node's own 4x4 rest transform, rebuilt only when its rotation or translation is replaced."""
        node = self.nodes[name]
        held = self.rest_matrices.get(name)
        if held is None or held[0] is not node.rotation or held[1] is not node.translation:
            held = self.rest_matrices[name] = (node.rotation, node.translation, to4(*self.local(name)))
        return held[2]

    def fk_order(self):
        """Nodes with every parent before its children, in the order a recursive walk visits them."""
        key = (tuple(self.nodes), tuple(self.parents.items()))
        if self.fk_orders is None or self.fk_orders[0] != key:
            order, seen = [], set()
            def visit(name):
                if name not in seen:
                    if name in self.parents: visit(self.parents[name])
                    seen.add(name); order.append((name, self.parents.get(name)))
            for name in self.nodes: visit(name)
            self.fk_orders = (key, order)
        return self.fk_orders[1]

    def fk(self, overrides):
        world = {}
        identity = np.eye(4)
        for name, parent in self.fk_order():
            local = to4(*overrides[name]) if overrides and name in overrides else self.rest_matrix(name)
            world[name] = (identity if parent is None else world[parent])@local
        return world

    def native_idle_camera(self, path):
        """Read the original game's standing eye, including its idle KF parents."""
        stream = self.nif.NiStream(); stream.load(path)
        names = [e.string_data for e in stream.root.extra_datas if isinstance(e, self.nif.NiStringExtraData)]
        t = next(float(time) for time, text in stream.root.extra_data.keys
                 if 'idle: start' in [x.strip().lower() for x in text.split('\n')])
        overrides = {}
        for name, controller in zip(names, stream.root.controllers):
            if name not in self.nodes or controller.data is None: continue
            rotation, translation = self.local(name)
            keys = controller.data.rotations
            if keys is not None and len(keys.keys):
                keys = keys.keys
                q = np.array([np.interp(t, keys[:, 0], keys[:, i]) for i in range(1, 5)])
                q /= norm(q)
                rotation = quat_matrix(q[[1, 2, 3, 0]])
            keys = controller.data.translations
            if keys is not None and len(keys.keys):
                keys = keys.keys
                translation = np.array([np.interp(t, keys[:, 0], keys[:, i]) for i in range(1, 4)])
            overrides[name] = rotation, translation
        return self.fk(overrides)['Camera'][:3, 3]

    def sample_kf(self, path, label, wanted):
        nif = self.nif
        stream = nif.NiStream(); stream.load(path)
        names = [e.string_data for e in stream.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
        t = next(float(time) for time, text in stream.root.extra_data.keys
                 if label in [x.strip().lower() for x in text.split('\n')])
        result = {}
        for name, controller in zip(names, stream.root.controllers):
            if not wanted(name) or controller.data is None or controller.data.rotations is None: continue
            keys = controller.data.rotations.keys
            if not len(keys): continue
            q = np.array([np.interp(t, keys[:, 0], keys[:, i]) for i in range(1, 5)])
            w, x, y, z = q/norm(q)
            result[name] = quat_matrix([x, y, z, w])
        return result

    # --- Dark Messiah viewmodel ------------------------------------------------------------
    def dm_resource(self, model, extension):
        dst = self.work/(model+extension)
        if not dst.exists():
            dst.parent.mkdir(parents=True, exist_ok=True)
            wanted = 'mm/models/'+('' if '/' in model else 'npc/player/')+model+extension
            for archive in reversed(archives(self.vpk)):
                found = next((e for n, e in entries(archive) if n.lower() == wanted), None)
                if found is not None: store(dst, extract(archive, found)); break
            else: raise FileNotFoundError(wanted)
        return dst

    def dm_model(self, model):
        if model not in self.models:
            self.models[model] = load_mdl(self.dm_resource(model, '.mdl'), SOURCEIO)
        return self.models[model]

    def dm_bones(self, model): return self.dm_model(model)[2]

    def item_idle(self, model):
        bones = self.dm_bones(model)
        bind = {}
        for bone in bones:
            local = to4(quat_matrix(bone.quat), np.asarray(bone.position))
            bind[bone.name] = bind[bones[bone.parent_id].name]@local if bone.parent_id >= 0 else local
        root = bones[0].name
        return self.dm_world(model, '@idle', 0.)[root]@np.linalg.inv(bind[root])

    def attachment(self, model, name):
        from SourceIO.library.models.mdl.structs.attachment import Attachment
        buf, header, bones, _ = self.dm_model(model)
        buf.seek(header.local_attachment_offset)
        for _ in range(header.local_attachment_count):
            at = Attachment.from_buffer(buf, header.version)
            if at.name == name:
                if model == 'first_basic':
                    checked = validate_attachment(at.name, bones[at.parent_bone].name)
                    if checked:
                        self.bone_correspondence.setdefault('attachments', {})[name] = checked
                m = np.array(at.matrix, float).reshape(3, 4)
                return to4(m[:, :3], m[:, 3])
        raise KeyError(name)

    def dm_clip(self, model, name):
        buf, header, bones, animations = self.dm_model(model)
        anim = next(a for a in animations if a.name == name)
        if anim.animblock_id or int(anim.flags) & 4: raise ValueError(f'Unsupported clip {model}:{name}')
        if not any(p['clip'] == name and p['model'] == model for p in self.provenance):
            self.provenance.append({'model': model, 'clip': name, 'fps': anim.fps, 'frames': anim.frame_count})
        return bones, anim.read_animations(buf, bones), anim

    def sequence_fade(self, model, clip):
        if model not in self.sequences:
            from SourceIO.library.models.mdl.structs.sequence import StudioSequence
            buf, header, _, animations = self.dm_model(model)
            buf.seek(header.local_sequence_offset)
            by_animation = {}
            for _ in range(header.local_sequence_count):
                sequence = StudioSequence.from_buffer(buf, header.version)
                for index in sequence.anim_desc_indices:
                    if 0 <= index < len(animations):
                        by_animation.setdefault(animations[index].name, sequence)
            self.sequences[model] = by_animation
        sequence = self.sequences[model].get(clip)
        return float(sequence.fade_in_time) if sequence else 0.

    def sequence_event_time(self, model, clip, event, fallback):
        self.sequence_fade(model, clip)  # populate the sequence metadata cache
        sequence = self.sequences[model].get(clip)
        if sequence:
            for entry in sequence.events:
                if entry.event == event:
                    return float(entry.cycle)*self.duration(model, clip)
        return fallback

    def duration(self, model, name):
        _, _, anim = self.dm_clip(model, name)
        return max(1/FPS, (anim.frame_count-1)/anim.fps)

    def dm_world(self, model, name, t):
        bones, tracks, anim = self.dm_clip(model, name)
        f = min(t*anim.fps, anim.frame_count-1)
        f0 = int(np.floor(f)); f1 = min(f0+1, anim.frame_count-1); u = f-f0
        out = {}
        for b in bones:
            track = tracks.get(b.name)
            if track is not None and norm(track['rot'][min(f0, len(track)-1)]) > 1e-6:
                q0, q1 = track['rot'][min(f0, len(track)-1)], track['rot'][min(f1, len(track)-1)]
                if np.dot(q0, q1) < 0: q1 = -q1
                q = (1-u)*q0+u*q1
                p = (1-u)*np.array(track['pos'][min(f0, len(track)-1)])+u*np.array(track['pos'][min(f1, len(track)-1)])
            else:
                q, p = np.array(b.quat, float), np.array(b.position, float)
            m = to4(quat_matrix(q), p)
            out[b.name] = out[bones[b.parent_id].name]@m if b.parent_id >= 0 else m
        return out

    # --- Retarget one instant --------------------------------------------------------------
    def to_mw(self, point):
        return self.physical_eye+self.scale*BASIS@(point-self.viewmodel_origin)

    def set_camera(self, dm):
        self.current_camera_add = (dm['SK_Camera_Eyes']@self.camera_attachment)[:3, 3]
        if self.camera_contract:
            # BaseViewModel::Spawn stores CameraAdd once. Its authored animation
            # root does not drive the physical camera. Actual kick getpos instead
            # matches the full-body MMeyes attachment exactly; that separate
            # translation is emitted on a carrier outside the pitch controller.
            self.viewmodel_origin = self.reference_camera
            self.physical_eye = self.base_eye+self.body_scale*BASIS@self.reference_camera
        else:
            # Without a body-camera contract, use the first-person skeleton eye.
            self.physical_eye = self.camera+self.scale*BASIS@(self.current_camera_add-self.reference_camera)
            self.viewmodel_origin = self.current_camera_add

    def body_eye_carrier(self, clip, time):
        body = self.dm_world('player_animations', clip, time)
        eye = (body['SK_Head']@self.attachment('player_basic', 'MMeyes'))[:3, 3]
        return self.body_scale*BASIS@(eye-self.source_base_eye)

    def shield_target(self, dm_shield):
        return self.to_mw(dm_shield[:3, 3])+BASIS@dm_shield[:3, :3]@self.shield_offset

    def weapon_target(self, dm_hand):
        handle = dm_hand@self.weapon_attachment@self.weapon_idle
        return self.to_mw(handle[:3, 3])

    def solve(self, dm, roll_state=None, weapon_grip=True):
        self.set_camera(dm)
        # Every animated node is keyed, like Morrowind's own clips: the first-person neck
        # RotateController composes onto a keyframed neck and would accumulate otherwise.
        overrides = {name: self.local(name) for name in self.keyed}
        overrides.update({name: (R, self.local(name)[1]) for name, R in self.grip.items()})
        world = self.fk(overrides)
        for side, names in SIDES.items():
            clav, upper, fore, hand = names['mw']
            _, dm_shoulder, dm_elbow, dm_hand = names['dm']
            neck = world[self.parents[clav]]
            Rc, _ = self.local(clav)
            shoulder = self.to_mw(dm[dm_shoulder][:3, 3])
            Rcw = neck[:3, :3]@Rc
            clav_world = shoulder-Rcw@self.local(upper)[1]
            overrides[clav] = (Rc, neck[:3, :3].T@(clav_world-neck[:3, 3]))
            L1 = norm(self.local(fore)[1]); L2 = norm(self.local(hand)[1])
            Rh = BASIS@dm[dm_hand][:3, :3]@self.hand_map[side]
            target = self.to_mw(dm[dm_hand][:3, 3]); pole = self.to_mw(dm[dm_elbow][:3, 3])
            if side == 'R' and weapon_grip:
                # The two rigs' hand joints are different grip anchors. Preserve
                # Morrowind's palm-to-hilt transform and its clenched fingers, but
                # solve the wrist so the actual hilt matches DM's RightHand handle.
                # Matching the hand joint alone still offsets the hilt because
                # the two rigs' attachment translations differ.
                target = self.weapon_target(dm[dm_hand])-Rh@self.weapon_bone[1]
            target = self.hand_target(side, dm, Rh, target, weapon_grip)
            if side == 'L' and self.shield:
                # Put the shield itself where DM holds it; the hand follows the grip offset.
                dm_shield = dm['SK_L_Wrist']@self.shield_attachment
                Rs = BASIS@dm_shield[:3, :3]@self.shield_map  # world rotation of Shield Bone
                Rsb, Tsb = self.shield_bone
                Rh_shield = Rs@Rsb.T
                target = self.shield_target(dm_shield)-Rh_shield@Tsb
            reach = target-shoulder; d = norm(reach)
            d = np.clip(d, abs(L1-L2)+1e-3, L1+L2-1e-3); axis = reach/norm(reach)
            target = shoulder+axis*d
            bend = pole-shoulder; bend = bend-axis*np.dot(bend, axis)
            normal = BASIS@dm[dm_elbow][:3, :3]@self.dm_hinge[side]
            bend = cross(axis, normal)
            if norm(bend) < 1e-6: bend = cross(axis, [0, 0, 1])
            bend /= norm(bend)
            a = (L1*L1-L2*L2+d*d)/(2*d); h = np.sqrt(max(L1*L1-a*a, 0))
            elbow = shoulder+axis*a+bend*h
            normal = cross(elbow-shoulder, target-elbow); normal /= norm(normal)
            hinge = self.hinge[side]
            # Bone axis +X points to the child; the hinge axis keeps its rest relation.
            hinge_perp = hinge-np.array([1, 0, 0])*hinge[0]; hinge_perp /= norm(hinge_perp)
            def oriented(direction):
                F = frame(direction, normal)
                B = frame(np.array([1., 0, 0]), hinge_perp)
                return F@B.T
            Ru = oriented(elbow-shoulder); Rf = oriented(target-elbow)
            if side == 'L' and self.shield:
                Rh = Rh_shield
            # Share the forearm/hand roll so the wrist does not candy-wrap.
            local_hand = Rf.T@Rh
            twist = np.arctan2(local_hand[2, 1]-local_hand[1, 2], local_hand[1, 1]+local_hand[2, 2])
            if roll_state is not None:
                # atan2 wraps a continuous wrist twist at +/-pi. Splitting that wrapped
                # angle between the forearm and hand would snap each bone by pi, even
                # though their composed hand orientation and IK endpoint stay correct.
                # Follow the full twist continuously before sharing it between bones.
                if side in roll_state:
                    previous = roll_state[side]
                    twist = previous + np.arctan2(np.sin(twist-previous), np.cos(twist-previous))
                roll_state[side] = twist
            roll = twist/2
            c, s = np.cos(roll), np.sin(roll)
            Rf = Rf@np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
            overrides[upper] = (Rcw.T@Ru, self.local(upper)[1])
            overrides[fore] = (Ru.T@Rf, self.local(fore)[1])
            overrides[hand] = (Rf.T@Rh, self.local(hand)[1])
        head = world['Bip01 Head']
        overrides['Camera'] = (self.local('Camera')[0], head[:3, :3].T@(self.physical_eye-head[:3, 3]))
        if self.camera_contract:
            overrides[CAMERA_CARRIER] = (np.eye(3), np.zeros(3))
        return overrides

    def hand_target(self, side, dm, hand_rotation, target, weapon_grip):
        """Weapon families override this to grip a second weapon or a staff shaft."""
        return target

    def finish_pose(self, pose, dm):
        """Weapon families key their item nodes here; self.source_clip gives the clock."""
        return pose

    def error(self, dm, overrides, weapon_grip=True):
        self.set_camera(dm)
        world = self.fk(overrides)
        errors = [norm(world['Weapon Bone'][:3, 3]-self.weapon_target(dm['SK_R_Hand'])) if weapon_grip
                  else norm(world['Bip01 R Hand'][:3, 3]-self.to_mw(dm['SK_R_Hand'][:3, 3]))]
        if self.shield:
            shield = self.shield_target(dm['SK_L_Wrist']@self.shield_attachment)
            errors.append(norm(world['Shield Bone'][:3, 3]-shield))
        else:
            errors.append(norm(world['Bip01 L Hand'][:3, 3]-self.to_mw(dm['SK_L_Hand'][:3, 3])))
        return max(errors)

    # --- KF output -------------------------------------------------------------------------
    def write_camera_skeleton(self, out):
        if self.camera_contract is None: return
        stream = self.nif.NiStream(); stream.load(self.skeleton_path)
        root = stream.root
        if (not np.allclose(root.rotation, np.eye(3), atol=1e-6)
                or not np.allclose(root.translation, 0., atol=1e-6) or abs(root.scale-1.) > 1e-6):
            raise ValueError('Camera pivot expects an identity first-person skeleton root')
        name = self.camera_contract['pivot_node']
        children = [node for node in root.children if getattr(node, 'name', None) != name]
        if any(getattr(node, 'name', None) == CAMERA_CARRIER for node in children):
            raise ValueError('Input skeleton already contains a camera carrier')
        root.children = [self.nif.NiNode(name=CAMERA_CARRIER, children=children)]
        root.children.append(self.nif.NiNode(name=name, translation=self.base_eye.astype(np.float32)))
        path = out/'meshes/brutal_fp/xbase_anim.1st.nif'; path.parent.mkdir(parents=True, exist_ok=True)
        stream.save(path)
        check = self.nif.NiStream(); check.load(path)
        pivot = next(node for node in check.objects_of_type(self.nif.NiNode) if node.name == name)
        assert np.allclose(pivot.translation, self.base_eye, atol=1e-5)
        carrier = next(node for node in check.objects_of_type(self.nif.NiNode) if node.name == CAMERA_CARRIER)
        assert any(node.name == 'ModelRoot' for node in carrier.children)
        assert carrier in check.root.children and pivot in check.root.children
    @staticmethod
    def blend_pose(first, second, weight, target_rotations=None):
        result = {}
        prime_quaternions([pose[name][0] for pose in (first, second) for name in second])
        for name in second:
            r0, t0 = first[name]; r1, t1 = second[name]
            q0, q1 = matrix_quat(r0), matrix_quat(r1)
            # A moving target can cross 180 degrees from the initial pose during a
            # fade. Rechoosing its shortest hemisphere against q0 every key flips
            # the interpolation path. Lift the target continuously through the fade.
            previous = q0 if target_rotations is None else target_rotations.get(name, q0)
            if np.dot(previous, q1) < 0: q1 = -q1
            if target_rotations is not None: target_rotations[name] = q1
            cosine = np.clip(np.dot(q0, q1), -1., 1.)
            theta = np.arccos(cosine)
            if theta < 1e-6:
                q = (1-weight)*q0+weight*q1
            elif np.pi-theta < 1e-6:
                # Preserve the incoming interpolation plane at an antipodal target.
                tangent = previous-q0*np.dot(q0, previous)
                if norm(tangent) < 1e-6:
                    axis = np.eye(4)[np.argmin(np.abs(q0))]
                    tangent = axis-q0*np.dot(q0, axis)
                tangent /= norm(tangent)
                q = np.cos(weight*theta)*q0+np.sin(weight*theta)*tangent
            else:
                q = (np.sin((1-weight)*theta)*q0+np.sin(weight*theta)*q1)/np.sin(theta)
            result[name] = (quat_matrix(q[[1, 2, 3, 0]]), (1-weight)*t0+weight*t1)
        return result

    def stance_pose(self, armed=True, blocking=False):
        if blocking:
            model, clip = (('first_animations_shield', 'anm_parry_sword_shield_idle') if self.shield
                           else ('first_animations_sword', 'anm_parry_sword_idle'))
        else:
            model, clip = 'first_animations_basic', ('anm_shield_idle' if self.shield else 'anm_sword_noshield_idle') if armed else '@idle01'
        shield = self.shield
        if not armed: self.shield = False
        pose = self.solve(self.dm_world(model, clip, 0.), {}, weapon_grip=armed)
        self.shield = shield
        return pose, self.sequence_fade(model, clip)

    def head_motion(self, clip, time):
        """Read the separate camera model and apply DM's bone-axis correction.

        Dark Messiah reads bone zero in entity space, negates its MatrixAngles
        pitch/roll, then composes QAngle(90,0,90)
        (the defaults of pouetyaw/pouetpitch/pouetroll). This is essential:
        the authored loss's 20-degree bone roll becomes a camera pitch jerk.
        Lua extras negate native X/Z/Y, so the corrected Source angles become
        Lua pitch/-yaw/-roll in radians.
        """
        matrix = self.dm_world('player_head_mvt', clip, time)['polySurface1']
        R = matrix[:3, :3]
        pitch = -np.arctan2(-R[2, 0], np.hypot(R[0, 0], R[1, 0]))
        yaw = np.arctan2(R[1, 0], R[0, 0])
        roll = -np.arctan2(R[2, 1], R[2, 2])
        sp, sy, sr = np.sin([pitch, yaw, roll]); cp, cy, cr = np.cos([pitch, yaw, roll])
        rotation = np.array([[cp*cy, sr*sp*cy-cr*sy, cr*sp*cy+sr*sy],
                             [cp*sy, sr*sp*sy+cr*cy, cr*sp*sy-sr*cy],
                             [-sp, sr*cp, cr*cp]])
        correction = np.array([[0., 1., 0.], [0., 0., -1.], [-1., 0., 0.]])
        R = rotation@correction
        angles = np.array([np.arctan2(-R[2, 0], np.hypot(R[0, 0], R[1, 0])),
                           np.arctan2(R[1, 0], R[0, 0]),
                           np.arctan2(R[2, 1], R[2, 2])])
        angles = np.arctan2(np.sin(angles), np.cos(angles))*[1., -1., -1.]
        # Dark Messiah also negates translation X/Y before
        # rotating by the view. This moves the eye, not the body/viewmodel mount.
        return matrix[:3, 3]*[-1., -1., 1.], angles

    def segment(self, model, clip, duration=None, loop=False, roll_state=None, start_pose=None, return_to=None, body_camera=None, head_camera=None, runtime_entry=False, loop_lead_in=0., dm_frames=None):
        natural = self.duration(model, clip)
        fade = self.sequence_fade(model, clip)
        period = (duration or natural) if loop else None
        self.pending.append(dict(model=model, clip=clip, natural=natural, loop=loop,
                                 fade_in=fade if start_pose is not None and not runtime_entry else 0.,
                                 runtime_fade_in=fade if runtime_entry else 0.,
                                 loop_period=period, loop_lead_in=loop_lead_in,
                                 body_camera=body_camera if self.camera_contract else None,
                                 head_camera=head_camera,
                                 return_fade=float(return_to[1]) if return_to else 0.))
        # A short transition (parry-in is .133s) can finish before its .2s Source
        # crossfade. Complete that fade while holding the source final pose before
        # entering the held loop, otherwise every loop would dip back to a half raise.
        duration = duration or max(natural, fade if start_pose is not None else 0.)
        if loop and loop_lead_in:
            duration = period+loop_lead_in
        times = np.linspace(0, duration, max(2, round(duration*FPS)+1))
        poses, worst, target_rotations = [], 0., {}
        runtime_arms = set()
        if runtime_entry:
            for name in self.nodes:
                ancestor = name
                while ancestor in self.parents:
                    if ancestor in ('Bip01 L Clavicle', 'Bip01 R Clavicle'):
                        runtime_arms.add(name)
                        break
                    ancestor = self.parents[ancestor]
        if roll_state is None: roll_state = {}
        for index, t in enumerate(times):
            source = ((t % period)*natural/period if loop_lead_in else t*natural/duration) if loop else min(t, natural)
            dm = dm_frames[index] if dm_frames is not None else self.dm_world(model, clip, source)
            weapon_grip = clip != '@idle01'
            self.source_clip = (model, clip, source)
            # The item is a separate Source entity. Its flip clock remains real
            # elapsed seconds even when a held arm loop uses a different period.
            self.source_item_time = t if loop else source
            try:
                pose = self.finish_pose(self.solve(dm, roll_state, weapon_grip=weapon_grip), dm)
            finally:
                del self.source_item_time
            worst = max(worst, self.error(dm, pose, weapon_grip=weapon_grip))
            if self.camera_contract and body_camera:
                pose[CAMERA_CARRIER] = (np.eye(3), self.body_eye_carrier(body_camera, source))
            if head_camera:
                # Loss head lasts longer than the arm break; keep its own clock.
                position, _ = self.head_motion(head_camera, source if loop else float(t))
                scale = self.body_scale if self.camera_contract else self.scale
                translation = scale*BASIS@position
                # Head movement offsets the final eye separately from the
                # body/MMeyes viewmodel mount. Camera is below the neck
                # pitch controller, so its local offset follows the view;
                # the kick's shared body carrier must remain neutral here.
                world = self.fk(pose)
                R, p = pose['Camera']
                pose['Camera'] = (R, p+world[self.parents['Camera']][:3, :3].T@translation)
            if start_pose is not None and fade > 0 and t < fade:
                baked = self.blend_pose(start_pose, pose, t/fade, target_rotations)
                # Only the native arm groups crossfade at runtime. Keep camera,
                # recoil carrier and other root tracks exactly as baked,
                # including the kick's motion.
                # An item with its own source flip clock must not spin through the arm fade.
                keep = runtime_arms | getattr(self, 'fade_exempt', set())
                pose = {name: value if name in keep else baked[name] for name, value in pose.items()}
            poses.append(pose)
        if return_to and return_to[1] > 0:
            target, fade = return_to
            tail = np.linspace(0, fade, max(2, round(fade*FPS)+1))[1:]
            last = poses[-1]
            poses += [self.blend_pose(last, target, float(t/fade)) for t in tail]
            times = np.concatenate([times, duration+tail])
        return times, poses, worst

    def held_loop(self, model, clip, duration, start_pose, roll_state, lead=None):
        """Play the entry fade once, then loop a full raw period at the settled phase."""
        if lead is None:
            lead = self.sequence_fade(model, clip)
            item_entry = getattr(self, 'loop_entry_time', lambda model, clip: 0.)(model, clip)
            lead = max(lead, item_entry)
        # The loop keys must land on emitted samples, after any one-off flip,
        # rather than interpolating across the last entry frame at every wrap.
        lead = float(np.ceil(lead*FPS-1e-5)/FPS)
        return self.segment(model, clip, duration=duration, loop=True, start_pose=start_pose,
                            roll_state=roll_state, loop_lead_in=lead), lead

    def write(self, out, group, segments, textkeys):
        nif = self.nif
        # Which DM clip and time each part of the group comes from, for reference renders.
        assert len(self.pending) == len(segments)
        gait = (out.name in ('brutal_fp', 'brutal_fp_shield', 'brutal_fp_staff', 'brutal_fp_dagger')
                and (group in ('idle', 'idle1h', 'brutal_sprint', 'brutal_sprint1h')
                     or group.startswith(('walk', 'run', 'sneak'))))
        fade = self.sequence_fade(self.pending[0]['model'], self.pending[0]['clip']) if gait else 0.
        entry_fades, extra_rules = [], []
        for (offset, _, _), src in zip(segments, self.pending):
            if src.get('runtime_fade_in', 0.) > 0:
                labels = [label for time, label in textkeys if abs(time-offset) < 1e-6
                          and (label == 'start' or label.endswith(' start'))]
                # At offset zero the weapon has both generic start and equip start.
                # Native playback uses the specific phase key.
                specific = [label for label in labels if label != 'start']
                if not labels:
                    raise ValueError(f'Runtime entry has no matching start key: {group} at {offset}')
                entry_fades.append((
                    specific[0] if specific else labels[0],
                    src['runtime_fade_in'],
                    src.get('runtime_easing', 'linear'),
                    src.get('runtime_clock', 'animation'),
                ))
            extra_rules.extend(src.get('extra_rules', ()))
        self.groups.setdefault(out.name, {})[group] = dict(
            marks=sorted([[float(t), label] for t, label in textkeys]),
            segments=[dict(offset=float(offset), duration=float(t[-1]), **src) for (offset, t, _), src in zip(segments, self.pending)],
            runtime_arm_fades=([dict(key='*', duration=fade, clock='simulation')] if gait else
                               [dict(key=key, duration=duration, clock=clock,
                                     **({'easing': easing} if easing != 'linear' else {}))
                                for key, duration, easing, clock in entry_fades]))
        self.pending = []
        times = np.concatenate([offset+t for offset, t, _ in segments])
        poses = [p for _, _, ps in segments for p in ps]
        names = sorted(set().union(*[p.keys() for p in poses]))
        stream = nif.NiStream()
        marks = sorted([(float(t), group+': '+label) for t, label in textkeys], key=lambda x: x[0])
        stream.root = nif.NiSequenceStreamHelper(extra_data=nif.NiTextKeyExtraData(keys=np.array(marks, dtype='<f,O')))
        duration = float(times[-1])
        for name in names:
            prime_quaternions([p[name][0] for p in poses])
            q = np.array([matrix_quat(p[name][0]) for p in poses], np.float32)
            for i in range(1, len(q)):
                if np.dot(q[i-1], q[i]) < 0: q[i] *= -1
            rot = np.column_stack([times, q]).astype(np.float32)
            tr = np.column_stack([times, [p[name][1] for p in poses]]).astype(np.float32)
            data = nif.NiKeyframeData(rotations=nif.NiRotData(keys=rot), translations=nif.NiPosData(keys=tr))
            if name == 'Shield Bone':
                scale = self.shield_scale if self.shield else 1.
                data.scales = nif.NiFloatData(keys=np.array([[0., scale], [duration, scale]], np.float32))
            stream.root.extra_datas.append(nif.NiStringExtraData(string_data=name))
            stream.root.controllers.append(nif.NiKeyframeController(data=data, start_time=0., stop_time=duration, flags=12))
        path = out/(group+'.kf'); stream.save(path)
        check = nif.NiStream(); check.load(path)
        assert len(list(check.root.controllers)) == len(names)
        print(f'{group}: {duration:.3f}s, {len(names)} bones, {len(marks)} events')
        if gait:
            self.write_gait_blend_rules(out, group, fade)
        elif entry_fades:
            rows = ['blending_rules:']
            for key, entry_fade, easing, clock in entry_fades:
                rows += ["  - from: '*'", f"    to: '{group}:{key}'", f'    easing: {easing}',
                         f'    duration: {entry_fade:.9g}', f'    clock: {clock}',
                         "  - from: ''", f"    to: '{group}:{key}'", '    easing: linear', '    duration: 0']
            for from_pat, to_key, fade_dur, easing, clock in extra_rules:
                rows += [f"  - from: '{from_pat}'", f"    to: '{group}:{to_key}'", f'    easing: {easing}',
                         f'    duration: {fade_dur:.9g}', f'    clock: {clock}']
            (out/(group+'.yaml')).write_text('\n'.join(rows)+'\n')

    @staticmethod
    def write_gait_blend_rules(out, group, fade):
        """Fade from the visible arms into a loop, without replaying the fade each cycle.

        Source stores this duration on the sequence. The native arm controller applies
        it at state changes; initial loading samples the clip directly from its first pose.
        """
        (out/(group+'.yaml')).write_text(
            "blending_rules:\n"
            "  - from: '*'\n"
            f"    to: '{group}'\n"
            "    easing: linear\n"
            f"    duration: {fade:.9g}\n"
            "  - from: ''\n"
            f"    to: '{group}'\n"
            "    easing: linear\n"
            "    duration: 0\n")

    def loop(self, out, group, model, clip):
        times, poses, err = self.segment(model, clip, loop=True)
        self.write(out, group, [(0, times, poses)], [(0, 'start'), (0, 'loop start'), (times[-1], 'loop stop'), (times[-1], 'stop')])
        return err

    def once(self, out, group, model, clip, marks):
        times, poses, err = self.segment(model, clip)
        self.write(out, group, [(0, times, poses)], [(0, 'start')]+list(marks)+[(times[-1], 'stop')])
        return err

    def kick(self, out, model, clip, height='medium'):
        armed = self.stance_pose()
        times, poses, err = self.segment(model, clip, start_pose=armed[0], return_to=armed,
                                        body_camera='@attack_kick_'+height, runtime_entry=True)
        self.write(out, 'brutal_kick1h' if height == 'medium' else 'brutal_kick1h_'+height, [(0, times, poses)],
                   [(0, 'start'), (.375, 'hit'), (times[-1], 'stop')])
        return err

    def weapon(self, out):
        kind = 'shield' if self.shield else 'noshield'
        attacks = 'first_animations_shield' if self.shield else 'first_animations_sword'
        segments, marks, offset, err = [], [(0, 'start')], 0., 0.
        armed, unarmed = self.stance_pose(), self.stance_pose(armed=False)
        for phase, clip in [('equip', f'anm_sword_{kind}_quickdraw'), ('unequip', f'anm_sword_{kind}_undraw')]:
            times, poses, e = self.segment('first_animations_basic', clip,
                start_pose=unarmed[0] if phase == 'equip' else armed[0],
                return_to=armed if phase == 'equip' else unarmed, runtime_entry=True); err = max(err, e)
            d = times[-1]; segments.append((offset, times, poses))
            event = 2100 if phase == 'equip' else 2101
            attach = self.sequence_event_time('first_animations_basic', clip, event, d*.4)
            marks += [(offset, phase+' start'), (offset+attach, phase+(' attach' if phase == 'equip' else ' detach')), (offset+d, phase+' stop')]
            offset += d+.05
        # Append the genuine opposite slash so all established attack timestamps
        # remain unchanged. This is a separate source animation, never a mirror.
        for attack, suffix in [('chop', '_T'), ('slash', '_L'), ('thrust', '_B'), ('slashright', '_R')]:
            prefix = f'anm_bigslash_sword_{kind}'+suffix
            roll_state = {}
            t1, p1, e1 = self.segment(attacks, prefix+'_in', roll_state=roll_state, start_pose=armed[0], runtime_entry=True)
            t2, p2, e2 = self.segment(attacks, prefix+'_out', roll_state=roll_state,
                start_pose=p1[-1], return_to=armed); err = max(err, e1, e2)
            wind, release = t1[-1], t2[-1]
            segments += [(offset, t1, p1), (offset+wind+.001, t2, p2)]
            hit = offset+wind+min(.30, release*.40); stop = offset+wind+.001+release
            marks += [(offset, attack+' start'), (offset+.15, attack+' min attack'), (offset+wind, attack+' max attack'),
                      (offset+wind+.001, attack+' min hit'), (hit, attack+' hit')]
            for strength in ('small', 'medium', 'large'):
                marks += [(hit+.001, attack+' '+strength+' follow start'), (stop, attack+' '+strength+' follow stop')]
            offset = stop+.05
        # The stationary charged swing has its own source clip. HitPow/impactdir stay with the
        # engine; this only adds playback keys after every established timestamp.
        prefix = f'anm_bigslash_sword_{kind}'
        roll_state = {}
        t1, p1, e1 = self.segment(attacks, prefix+'_in', roll_state=roll_state, start_pose=armed[0], runtime_entry=True)
        t2, p2, e2 = self.segment(attacks, prefix+'_out', roll_state=roll_state,
            start_pose=p1[-1], return_to=armed); err = max(err, e1, e2)
        wind, release = t1[-1], t2[-1]
        segments += [(offset, t1, p1), (offset+wind+.001, t2, p2)]
        hit = offset+wind+self.sequence_event_time(attacks, prefix+'_out', 2102, min(.30, release*.40))
        stop = offset+wind+.001+release
        marks += [(offset, 'neutral start'), (offset+.15, 'neutral min attack'), (offset+wind, 'neutral max attack'),
                  (offset+wind+.001, 'neutral min hit'), (hit, 'neutral hit')]
        for strength in ('small', 'medium', 'large'):
            marks += [(hit+.001, 'neutral '+strength+' follow start'), (stop, 'neutral '+strength+' follow stop')]
        offset = stop+.05
        fast = [(f'fast{v.lower()}', f'anm_fastattack_sword_{kind}_{v}', kind_hit, False)
                for v, kind_hit in zip('ABCDE', ('slashright', 'slash', 'slashright', 'slash', 'slashright'))]
        fast += [('fastenda', f'anm_fastattack_sword_{kind}_end', None, False)]
        fast += [(f'trans{v.lower()}', f'anm_transition_sword_{kind}_{v}', None, False) for v in 'ABCDE']
        offset, e = self.fast_sections(segments, marks, offset, attacks, armed, fast); err = max(err, e)
        # A charged swing stopped by a wall (layer_bounce_sword_*: neutral, _L, _R only).
        for key, suffix in (('bounce', ''), ('bouncel', '_L'), ('bouncer', '_R')):
            times, poses, e = self.bounce_section(attacks, f'anm_bigslash_sword_{kind}{suffix}_out',
                                                  f'anm_bounce_sword_{kind}{suffix}', armed); err = max(err, e)
            segments.append((offset, times, poses))
            marks += [(offset, key+' start'), (offset+times[-1], key+' stop')]
            offset += times[-1]+.05
        marks.append((offset-.05, 'stop'))
        self.write(out, 'weapononehand', segments, marks)
        return err

    def bounce_section(self, model, swing, bounce, armed):
        """Play the authored recoil immediately after an actual blade contact.

        The source bounce holds almost the same extended pose for 27 frames (neutral),
        25 frames (_L), or 9 frames (_R). Replaying that hold after our real contact makes
        the blade appear embedded in the wall. Trim only that source lead-in, retaining
        the remaining motion at 1x and the original hand/weapon relationship. The native
        arms crossfade from the actual contact pose in .06 s; no swing continuation is
        baked over the recoil.
        """
        if self.untrimmed_bounce_layout:
            return self.layered_bounce_section(model, swing, bounce, armed)
        first = 9 if bounce.endswith('_R') else 25 if bounce.endswith('_L') else 27
        times, poses, error = self.segment(model, bounce, roll_state={}, start_pose=armed[0], runtime_entry=True)
        source_start = first/FPS
        first = int(np.argmin(np.abs(times-source_start)))
        self.pending[-1].update(runtime_fade_in=.06, source_start=source_start,
                                source_natural=self.pending[-1]['natural'],
                                natural=float(times[-1]-times[first]),
                                adaptation='Remove the source leading hold; authored recoil tail at 1x.')
        return times[first:]-times[first], poses[first:], error

    def layered_bounce_section(self, model, swing, bounce, armed):
        """The untrimmed recoil as Dark Messiah layers it over the continuing swing.

        Emitted first and then trimmed in place by replace_bounce_sections(), which
        keeps the section starts, so later sections land on their final timestamps.
        """
        hit = self.sequence_event_time(model, swing, 2102, None)
        if hit is None: raise ValueError('Swing has no hit event: '+swing)
        roll_state = {}
        ts, ps, e1 = self.segment(model, swing, roll_state=roll_state, start_pose=None)
        self.pending.pop()
        tb, pb, e2 = self.segment(model, bounce, roll_state={}, start_pose=armed[0], runtime_entry=True)
        poses = []
        for t, pose in zip(tb, pb):
            weight = float(np.clip((t-.2)/.3, 0., 1.))
            i = min(len(ts)-1, int(round((hit+t)*FPS)))
            poses.append(self.blend_pose(ps[i], pose, weight) if weight < 1. else pose)
        return tb, poses, max(e1, e2)

    def replace_bounce_sections(self, output):
        """Replace only bounce keys in existing sword/shield groups, preserving other keys.

        Existing section starts remain fixed. Stops move to the end
        of the recoil, so subsequent sections keep their established timestamps.
        """
        import re
        report = {}
        for shield, folder in ((False, 'brutal_fp'), (True, 'brutal_fp_shield')):
            self.shield = shield
            kind = 'shield' if shield else 'noshield'
            model = 'first_animations_shield' if shield else 'first_animations_sword'
            path = output/'Animations'/folder/'weapononehand.kf'
            stream = self.nif.NiStream(); stream.load(path)
            extra = next(e for e in stream.root.extra_datas if isinstance(e, self.nif.NiTextKeyExtraData))
            labels = {str(label): float(time) for time, label in extra.keys}
            sections = []
            self.pending = []
            armed = self.stance_pose()
            for key, suffix in (('bounce', ''), ('bouncel', '_L'), ('bouncer', '_R')):
                start = labels['weapononehand: '+key+' start']
                stop = labels['weapononehand: '+key+' stop']
                times, poses, error = self.bounce_section(model, f'anm_bigslash_sword_{kind}{suffix}_out',
                                                         f'anm_bounce_sword_{kind}{suffix}', armed)
                sections.append(dict(key=key, start=start, old_stop=stop,
                                     times=times+start, poses=poses, error=error,
                                     source=dict(self.pending[-1])))
            names = [e.string_data for e in stream.root.extra_datas if isinstance(e, self.nif.NiStringExtraData)]
            unchanged = 0
            for name, controller in zip(names, stream.root.controllers):
                for field in ('rotations', 'translations'):
                    data = getattr(controller.data, field)
                    before = data.keys.copy()
                    keep = np.ones(len(before), dtype=bool)
                    added = []
                    for section in sections:
                        keep &= (before[:, 0] < section['start']-1e-5) | (before[:, 0] > section['old_stop']+1e-5)
                        values = [matrix_quat(p[name][0]) if field == 'rotations' else p[name][1]
                                  for p in section['poses']]
                        added.append(np.column_stack([section['times'], values]).astype(before.dtype))
                    combined = np.concatenate([before[keep]]+added)
                    data.keys = combined[np.argsort(combined[:, 0], kind='stable')]
                    # Compare the exact bytes of every preserved channel key.
                    after_keep = np.ones(len(data.keys), dtype=bool)
                    for section in sections:
                        after_keep &= (data.keys[:, 0] < section['start']-1e-5) | (data.keys[:, 0] > section['old_stop']+1e-5)
                    if before[keep].tobytes() != data.keys[after_keep].tobytes():
                        raise ValueError('Non-bounce keys changed: '+folder+':'+name+':'+field)
                    unchanged += int(keep.sum())
            marks = []
            for time, label in extra.keys:
                for section in sections:
                    if label == 'weapononehand: '+section['key']+' stop':
                        time = float(section['times'][-1])
                        break
                marks.append((float(time), label))
            preserved_marks = [(float(time), str(label)) for time, label in extra.keys
                               if not any(label == 'weapononehand: '+s['key']+' stop' for s in sections)]
            extra.keys = np.array(sorted(marks, key=lambda item: item[0]), dtype='<f,O')
            if preserved_marks != [(float(time), str(label)) for time, label in extra.keys
                                   if not any(label == 'weapononehand: '+s['key']+' stop' for s in sections)]:
                raise ValueError('Non-bounce text keys changed: '+folder)
            yaml = path.with_suffix('.yaml')
            text = yaml.read_text()
            text, changes = re.subn(r"(- from: '\*'\n\s+to: 'weapononehand:bounce[lr]? start'\n\s+easing: linear\n\s+duration:) [^\n]+",
                                    r'\g<1> 0.06', text)
            if changes != 3:
                raise ValueError('Missing bounce blend rules: '+str(yaml))
            stream.save(path)
            yaml.write_text(text)
            report[folder] = dict(nonbounce_channel_keys_byte_identical=unchanged,
                nonbounce_text_keys_unchanged=True,
                sections=[{k: (v.tolist() if isinstance(v, np.ndarray) else v) for k, v in section.items()
                           if k not in ('poses', 'times')} | {'stop': float(section['times'][-1])} for section in sections])
        (output/'wall-bounce-recoil-import.json').write_text(json.dumps(dict(
            converter='import_viewmodel.py --bounce-only', derived_from_game_files=True,
            runtime_fade=.06, report=report, clips=self.provenance), indent=2)+'\n')

    def fast_sections(self, segments, marks, offset, model, armed, clips):
        """Append the short click attacks, their ends and the transitions into a charged swing.

        Each is one source clip with its source fade at runtime; it blends from whichever chain
        link came before, so no return-to-idle tail is baked. The hit key names the logical swing
        from first_basic.qct impactdir (Y+1 slashright, Y-1 slash, X thrust); " left" marks the
        dagger clips that strike with the left hand.
        """
        worst = 0.
        for key, clip, strike, left in clips:
            times, poses, error = self.segment(model, clip, roll_state={}, start_pose=armed[0], runtime_entry=True)
            worst = max(worst, error)
            segments.append((offset, times, poses))
            marks.append((offset, key+' start'))
            if strike:
                swing = self.sequence_event_time(model, clip, 2107, 0.)
                hit = self.sequence_event_time(model, clip, 2102, None)
                if hit is None: raise ValueError('Fast attack has no hit event: '+clip)
                marks += [(offset+swing, key+' swing'),
                          (offset+hit, f'{key} {strike} hit'+(' left' if left else ''))]
            marks.append((offset+times[-1], key+' stop'))
            offset += times[-1]+.05
        return offset, worst

    def _smooth_shield_return(self, model, clip, times):
        """Ease a truncated shield-arm hold-and-snap tail cleanly across its trailing frames.

        In Source's anm_parry_sword_shield_B, the right arm returns smoothly by frame 7
        while the left shield arm holds ~28.8 deg through frame 6 (0.200s), snaps straight
        to guard at frame 7, and sits idle across frames 7..10. Reconstruct the left-arm
        recovery from frame 6 to frame 10 so both impact timing and clip duration stay exact.
        """
        natural = self.duration(model, clip)
        frames = [{name: mat.copy() for name, mat in self.dm_world(model, clip, min(float(t), natural)).items()}
                  for t in times]
        # Only rewrite trailing frames when the left hand is flat idle from frame 7 onward
        # while still displaced at frame 6.
        if len(frames) < 8:
            return frames
        end_rot, end_pos = frames[-1]['SK_L_Hand'][:3, :3], frames[-1]['SK_L_Hand'][:3, 3]
        hold_rot = np.degrees(np.arccos(np.clip((np.trace(frames[6]['SK_L_Hand'][:3, :3].T @ end_rot) - 1.) * .5, -1., 1.)))
        tail_rot = max(np.degrees(np.arccos(np.clip((np.trace(frames[k]['SK_L_Hand'][:3, :3].T @ end_rot) - 1.) * .5, -1., 1.)))
                       for k in range(7, len(frames)))
        if hold_rot < 20. or tail_rot > .1 or float(norm(frames[6]['SK_L_Hand'][:3, 3] - end_pos)) < 4.:
            return frames
        left_bones = [name for name in frames[0] if name.startswith('SK_L_') or name == 'Leftshield']
        span = float(len(frames) - 1 - 6)
        for k in range(7, len(frames) - 1):
            u = (k - 6) / span
            s = float(u * u * (3. - 2. * u))
            for name in left_bones:
                M0, M1 = frames[6][name], frames[-1][name]
                q0, q1 = matrix_quat(M0[:3, :3]), matrix_quat(M1[:3, :3])
                if float(np.dot(q0, q1)) < 0.:
                    q1 = -q1
                dot = float(np.clip(np.dot(q0, q1), -1., 1.))
                if dot > .99999:
                    q = (1. - s) * q0 + s * q1
                    q /= norm(q)
                else:
                    th = float(np.arccos(dot))
                    q = (np.sin((1. - s) * th) * q0 + np.sin(s * th) * q1) / np.sin(th)
                M = np.eye(4)
                M[:3, :3] = quat_matrix([q[1], q[2], q[3], q[0]])
                M[:3, 3] = (1. - s) * M0[:3, 3] + s * M1[:3, 3]
                frames[k][name] = M
        return frames

    def block(self, out):
        model = 'first_animations_shield'
        roll_state = {}
        armed, held = self.stance_pose(), self.stance_pose(blocking=True)
        in_duration = self.duration(model, 'anm_parry_sword_shield_in')
        t1, p1, e1 = self.segment(model, 'anm_parry_sword_shield_in', duration=in_duration, roll_state=roll_state)
        self.pending[-1].update(runtime_fade_in=self.sequence_fade(model, 'anm_parry_sword_shield_in'),
                                runtime_easing='smoothstep', runtime_clock='simulation',
                                extra_rules=[('brutal_blockhit*', 'start', 0., 'linear', 'simulation')])
        # The authored shield raise already arrives at the guard pose, so enter the
        # held loop directly without a static pre-loop hold.
        (t2, p2, e2), lead = self.held_loop(model, 'anm_parry_sword_shield_idle', 1., p1[-1], roll_state, lead=0.)
        # Source's shield_out clip is a static overlay placeholder; its sequence fade
        # lowers the guard immediately back to the armed idle.
        out_duration = self.duration(model, 'anm_parry_sword_shield_out')
        lower_fade = self.sequence_fade(model, 'anm_parry_sword_shield_out') or out_duration
        t3 = np.linspace(0, lower_fade, max(2, round(lower_fade*FPS)+1))
        dm_out = self.dm_world(model, 'anm_parry_sword_shield_out', 0.)
        # Lowering removes the static guard overlay. Blend once to the base idle
        # at runtime, from the pose actually displayed when the button is released.
        p3 = [armed[0] for _ in t3]
        e3 = self.error(dm_out, p2[-1], weapon_grip=True)
        self.pending.append(dict(
            model=model, clip='anm_parry_sword_shield_out', natural=out_duration, loop=False,
            fade_in=0., runtime_fade_in=lower_fade, runtime_easing='smoothstep', runtime_clock='simulation',
            loop_period=None, loop_lead_in=0., body_camera=None, head_camera=None, return_fade=lower_fade))
        hold = t1[-1]+.001; lower = hold+t2[-1]+.001; end = lower+t3[-1]
        self.write(out, 'brutal_block', [(0, t1, p1), (hold, t2, p2), (lower, t3, p3)],
                   [(0, 'start'), (.0001, 'shield overlay'), (hold+lead, 'loop start'), (hold+t2[-1], 'loop stop'), (lower, 'lower start'), (end, 'stop')])
        err = max(e1, e2, e3)
        for i, v in enumerate('ABC', 1):
            clip = 'anm_parry_sword_shield_'+v
            natural = self.duration(model, clip)
            times = np.linspace(0, natural, max(2, round(natural*FPS)+1))
            dm_frames = self._smooth_shield_return(model, clip, times) if v == 'B' else None
            times, poses, error = self.segment(model, clip, duration=natural, dm_frames=dm_frames)
            # Source attaches parry_sword_shield_{A,B,C} with AddLayeredSequence(blendIn=0, weight=1);
            # start/end already match held guard, while chained impacts crossfade cleanly.
            self.pending[-1].update(runtime_fade_in=.04, runtime_easing='sineOut', runtime_clock='simulation',
                                    extra_rules=[('brutal_block:start', 'start', 0., 'linear', 'simulation')])
            self.write(out, f'brutal_blockhit{i}', [(0, times, poses)], [(0, 'start'), (times[-1], 'stop')])
            err = max(err, error)
        # Shield bash from the raised guard (layer_parry_sword_shield_bash): it returns to
        # the held guard. Its source hit event 2102 sits at cycle 0.2368.
        clip = 'anm_parry_sword_shield_bash'
        times, poses, error = self.segment(model, clip, start_pose=held[0], return_to=held, runtime_entry=True)
        hit = self.sequence_event_time(model, clip, 2102, times[-1]*.2368)
        self.write(out, 'brutal_shieldbash', [(0, times, poses)], [(0, 'start'), (hit, 'hit'), (times[-1], 'stop')])
        return max(err, error)

    def sequence_events(self, model, clip):
        self.sequence_fade(model, clip)  # populate the sequence metadata cache
        sequence = self.sequences[model].get(clip)
        d = self.duration(model, clip)
        return [(int(e.event), float(e.cycle)*d) for e in sequence.events] if sequence else []

    def parry(self, out):
        # Sword-only guard (layer_parry_sword_noshield_*). It shares the shield guard's group
        # names: OpenMW loads this folder without a shield and the _shield folder with one.
        model = 'first_animations_sword'
        roll_state = {}
        armed, held = self.stance_pose(), self.stance_pose(blocking=True)
        t1, p1, e1 = self.segment(model, 'anm_parry_sword_in', roll_state=roll_state, start_pose=armed[0], runtime_entry=True)
        (t2, p2, e2), lead = self.held_loop(model, 'anm_parry_sword_idle', 1., p1[-1], roll_state)
        t3, p3, e3 = self.segment(model, 'anm_parry_sword_out', roll_state=roll_state,
            start_pose=p2[-1], return_to=armed)
        self.pending[-1].update(runtime_fade_in=.08, runtime_easing='sineOut', runtime_clock='simulation')
        hold = t1[-1]+.001; lower = hold+t2[-1]+.001; end = lower+t3[-1]
        self.write(out, 'brutal_block', [(0, t1, p1), (hold, t2, p2), (lower, t3, p3)],
                   [(0, 'start'), (hold+lead, 'loop start'), (hold+t2[-1], 'loop stop'), (lower, 'lower start'), (end, 'stop')])
        err = max(e1, e2, e3)
        # Parry_Parry_Sword plays one of A/B/C at random over the held guard.
        for i, v in enumerate('ABC', 1):
            times, poses, error = self.segment(model, 'anm_parry_sword_'+v, start_pose=held[0], return_to=held, runtime_entry=True)
            self.write(out, f'brutal_blockhit{i}', [(0, times, poses)], [(0, 'start'), (times[-1], 'stop')])
            err = max(err, error)
        # Parry_Break_Sword: the guard is knocked open and returns to the armed idle.
        times, poses, error = self.segment(model, 'anm_state_sword_parry_breaked', start_pose=held[0], return_to=armed, runtime_entry=True)
        self.write(out, 'brutal_parrybreak', [(0, times, poses)], [(0, 'start'), (times[-1], 'stop')])
        return max(err, error)

    def clinch(self, out):
        # Dark Messiah's Weapon_Vs_Weapon starts the right idle directly.
        # Its _in clip exists but is not used by that state. The contest ends
        # after one 44-frame idle; it is deliberately not a repeating loop.
        model = 'first_animations_sword'
        held = self.stance_pose(blocking=True)
        times, poses, error = self.segment(model, 'anm_sword_versus_noshield_r_idle',
            start_pose=held[0], head_camera='@layer_sword_versus_noshield_r_idle')
        final_pose = poses[-1]
        self.write(out, 'brutal_clinch', [(0, times, poses)], [(0, 'start'), (times[-1], 'stop')])
        errors = [error]
        for group, arms, head, duration in [
            ('brutal_clinch_win', 'anm_sword_versus_noshield_r_out',
             '@layer_sword_versus_noshield_r_out', None),
            ('brutal_clinch_lose', 'anm_state_sword_parry_breaked',
             '@layer_sword_versus_noshield_out_loose', 40/30)]:
            # Loss uses the normal sword parry break (31 frames at 24fps)
            # with a distinct 41-frame head animation, not an invented arm clip.
            times, poses, error = self.segment(model, arms, duration=duration,
                start_pose=final_pose, head_camera=head)
            self.write(out, group, [(0, times, poses)], [(0, 'start'), (times[-1], 'stop')])
            errors.append(error)
        return max(errors)

    def write_clinch_head(self, output):
        # Generated from the user's game; Lua loads it on demand.
        clips = {}
        for group, name in [('brutal_clinch', '@layer_sword_versus_noshield_r_idle'),
                            ('brutal_clinch_win', '@layer_sword_versus_noshield_r_out'),
                            ('brutal_clinch_lose', '@layer_sword_versus_noshield_out_loose')]:
            _, _, anim = self.dm_clip('player_head_mvt', name)
            clips[group] = dict(source=name, fps=anim.fps, frames=[
                self.head_motion(name, frame/anim.fps)[1].tolist() for frame in range(anim.frame_count)])
        dest = output/'scripts/brutal_assets/clinch_head.lua'
        dest.parent.mkdir(parents=True, exist_ok=True)
        lines = ['-- Dark Messiah camera animation, derived from the user\'s own game files.',
                 '-- Frames are Lua camera extra pitch, yaw, roll in radians.', 'return {']
        for group, data in clips.items():
            lines.append(f'  {group} = {{fps = {data["fps"]:.9g}, frames = {{')
            lines += ['    {'+', '.join(f'{value:.9g}' for value in row)+'},' for row in data['frames']]
            lines.append('  }},')
        dest.write_text('\n'.join(lines+['}', '']))
        return clips

    def rush(self, out):
        # layer_combo_sword_rush_*: two strikes, hit events 2102 (event0) and 2139 (event1);
        # 2107 starts each swing sound.
        model, clip = (('first_animations_shield', 'anm_combo_sword_rush_shield') if self.shield
                       else ('first_animations_sword', 'anm_combo_sword_rush_noshield'))
        armed = self.stance_pose()
        times, poses, err = self.segment(model, clip, start_pose=armed[0], return_to=armed, runtime_entry=True)
        events = self.sequence_events(model, clip)
        swings = [t for e, t in events if e == 2107]
        hits = [t for e, t in events if e == 2102][:1]+[t for e, t in events if e == 2139][:1]
        if len(swings) != 2 or len(hits) != 2: raise ValueError(f'Unexpected rush events: {events}')
        self.write(out, 'brutal_rush', [(0, times, poses)],
                   [(0, 'start'), (swings[0], 'swing'), (hits[0], 'hit'), (swings[1], 'swing2'), (hits[1], 'hit2'),
                    (times[-1], 'stop')])
        return err

    def fall_attack(self, out):
        # layer_combo_sword_fall_*: jump_in raises the sword, idle holds it while airborne,
        # out strikes on landing (hit event 2102). The engine loops the idle until the ground.
        kind = 'shield' if self.shield else 'noshield'
        model = 'first_animations_shield' if self.shield else 'first_animations_sword'
        prefix = f'anm_combo_sword_fall_{kind}'
        roll_state = {}
        armed = self.stance_pose()
        t1, p1, e1 = self.segment(model, prefix+'_jump_in', roll_state=roll_state, start_pose=armed[0], runtime_entry=True)
        (t2, p2, e2), lead = self.held_loop(model, prefix+'_idle', .5, p1[-1], roll_state)
        t3, p3, e3 = self.segment(model, prefix+'_out', roll_state=roll_state, start_pose=p2[-1], return_to=armed)
        events = dict((e, t) for e, t in reversed(self.sequence_events(model, prefix+'_out')))
        if 2102 not in events: raise ValueError('Missing falling-attack hit event')
        hold = t1[-1]+.001; strike = hold+t2[-1]+.001; end = strike+t3[-1]
        self.write(out, 'brutal_fallattack', [(0, t1, p1), (hold, t2, p2), (strike, t3, p3)],
                   [(0, 'start'), (hold+lead, 'loop start'), (hold+t2[-1], 'loop stop'), (strike, 'out start'),
                    (strike+events.get(2107, 0.), 'swing'), (strike+events[2102], 'hit'), (end, 'stop')])
        return max(e1, e2, e3)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--vpk-root', type=Path, required=True)
    ap.add_argument('--work-dir', type=Path, required=True)
    ap.add_argument('--skeleton', type=Path, required=True, help='Morrowind meshes/xbase_anim.1st.nif')
    ap.add_argument('--grip-animation', type=Path, required=True, help='Morrowind meshes/xbase_anim.1st.kf')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--shield-normal', type=float, choices=(1., -1.), default=-1., help='Sign of the raw DM shield outward normal (+/-Y before @idle)')
    ap.add_argument('--shield-mesh', type=Path, help='Native equipped shield NIF; align grip/center while preserving its native size')
    ap.add_argument('--camera-contract', type=Path, help='Measured body-camera contract JSON; emits absolute physical eye and native pitch-pivot skeleton')
    ap.add_argument('--native-eye-height', action='store_true', help='Calibrate standing height from original Morrowind idle Camera; preserve camera-relative arms and DM recoil')
    ap.add_argument('--weapon-only', action='store_true')
    ap.add_argument('--untrimmed-bounce-layout', action='store_true',
                    help='Emit the untrimmed layered wall recoil; follow with --bounce-only to trim it in place')
    ap.add_argument('--bounce-only', action='store_true', help='Replace only bounce sections in an existing copied sword/shield data directory')
    ap.add_argument('--clinch-only', action='store_true', help='Emit only the three additive sword-clinch clips and camera curves')
    size = ap.add_mutually_exclusive_group()
    size.add_argument('--shield-fit-size', action='store_true', help='Explicitly resize the native shield to match DM guard width/height')
    size.add_argument('--shield-keep-size', action='store_true', help='Explicitly retain native shield size (the default)')
    a = ap.parse_args()
    if (a.shield_fit_size or a.shield_keep_size) and a.shield_mesh is None:
        ap.error('shield size options require --shield-mesh')
    a.work_dir.mkdir(parents=True, exist_ok=True)
    vm = Viewmodel(a.vpk_root, a.work_dir, a.skeleton, a.grip_animation, a.shield_normal, a.shield_mesh, not a.shield_fit_size, a.camera_contract, a.native_eye_height)
    vm.untrimmed_bounce_layout = a.untrimmed_bounce_layout
    print(f'scale {vm.scale:.4f} Morrowind units per DM inch')
    if a.bounce_only:
        vm.replace_bounce_sections(a.output)
        return
    if a.weapon_only:
        # Only weapononehand changes when the attack set grows; every other group stays unchanged.
        report = {}
        for shield, folder in ((False, 'brutal_fp'), (True, 'brutal_fp_shield')):
            vm.shield = shield
            out = a.output/'Animations'/folder; out.mkdir(parents=True, exist_ok=True)
            report[folder] = vm.weapon(out)
        (a.output/'viewmodel-weapon-import.json').write_text(json.dumps(dict(
            converter='import_viewmodel.py --weapon-only', max_hand_error=report, clips=vm.provenance, groups=vm.groups,
            derived_from_game_files=True), indent=2)+'\n')
        return
    if a.clinch_only:
        out = a.output/'Animations/brutal_fp'; out.mkdir(parents=True, exist_ok=True)
        error = vm.clinch(out)
        head = vm.write_clinch_head(a.output)
        sources = [a.skeleton, a.grip_animation]+[a.work_dir/(model+'.mdl') for model in vm.models]
        if a.camera_contract: sources.append(a.camera_contract)
        outputs = list(out.glob('brutal_clinch*.kf'))+[a.output/'scripts/brutal_assets/clinch_head.lua']
        (a.output/'viewmodel-clinch-import.json').write_text(json.dumps(dict(
            converter='import_viewmodel.py --clinch-only', derived_from_game_files=True,
            scale=vm.scale, camera_contract=vm.camera_contract, height_calibration=vm.height_calibration,
            clips=vm.provenance, groups=vm.groups, head=head, max_hand_error=error,
            inputs={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
            outputs={path.relative_to(a.output).as_posix():hashlib.sha256(path.read_bytes()).hexdigest() for path in outputs},
            head_axes='Source QAngle(-bonePitch,boneYaw,-boneRoll) * QAngle(90,0,90); Lua radians = (pitch,-yaw,-roll) after decomposition.',
            notes='Original one-shot sword versus idle/out; loss is original parry-break arms and versus-loss head. Final-eye translation uses Camera only with X/Y source sign correction; arm/body carrier stays neutral. Lua consumes rotation.'
        ), indent=2)+'\n')
        return
    basic, err = 'first_animations_basic', {}
    # Two complete sets with the same group names: OpenMW adds brutal_fp_shield only
    # while the player carries a shield, as DM switches to its *_sword_shield clips.
    for shield, folder, clips in [
        (False, 'brutal_fp', dict(idle='anm_sword_noshield_idle', walk='@walk_sword', run='@run_sword',
                                       sprint='@Megasprint_sword', kick='anm_sword_noshield_kick_medium')),
        (True, 'brutal_fp_shield', dict(idle='anm_shield_idle', walk='@walk_sword_shield', run='@run_sword_shield',
                                             sprint='@Megasprint_sword_shield', kick='anm_sword_shield_kick_medium'))]:
        vm.shield = shield
        out = a.output/'Animations'/folder; out.mkdir(parents=True, exist_ok=True)
        tag = folder+':'
        err[tag+'idle1h'] = vm.loop(out, 'idle1h', basic, clips['idle'])
        for mode, clip in [('walk', clips['walk']), ('run', clips['run']), ('sneak', clips['walk'])]:
            for direction in ('forward', 'back', 'left', 'right'):
                err[tag+mode+direction+'1h'] = vm.loop(out, mode+direction+'1h', basic, clip)
        # Shift sprint: the engine plays it in place of the forward run.
        err[tag+'brutal_sprint1h'] = vm.loop(out, 'brutal_sprint1h', basic, clips['sprint'])
        err[tag+'weapononehand'] = vm.weapon(out)
        err[tag+'brutal_kick1h'] = vm.kick(out, basic, clips['kick'])
        # mm_player_minangle_to_medium_kick: a view pitched down 50 degrees kicks low.
        err[tag+'brutal_kick1h_low'] = vm.kick(out, basic, clips['kick'].replace('_medium', '_low'), 'low')
        err[tag+'brutal_rush'] = vm.rush(out)
        err[tag+'brutal_fallattack'] = vm.fall_attack(out)
        if shield: err[tag+'brutal_block'] = vm.block(out)
        else:
            err[tag+'brutal_block'] = vm.parry(out)
            err[tag+'brutal_clinch'] = vm.clinch(out)
            err[tag+'idle'] = vm.loop(out, 'idle', basic, '@idle01')
            err[tag+'brutal_sprint'] = vm.loop(out, 'brutal_sprint', basic, '@Megasprint')
    for group, e in err.items(): print(f'  max hand error {group}: {e:.3f}')
    vm.write_camera_skeleton(a.output)
    vm.write_clinch_head(a.output)
    (a.output/'viewmodel-import.json').write_text(json.dumps({
        'converter': 'import_viewmodel.py', 'scale': vm.scale,
        'bone_correspondence': vm.bone_correspondence,
        'reference_camera_add': vm.reference_camera.tolist(),
        'camera_contract': vm.camera_contract,
        'height_calibration': vm.height_calibration,
        'camera_carrier': {'node': CAMERA_CARRIER, 'source': 'player_basic/MMeyes',
                           'clips': ['player_animations/@attack_kick_medium'],
                           'camera_add': 'stored reference; not the animated viewmodel root'} if vm.camera_contract else None,
        'weapon_grip': {'source_attachment': 'first_basic/RightHand',
                        'source_item': 'items/weapons/sword_long/sword_long/@idle',
                        'native_attachment': 'Weapon Bone',
                        'source_translation': vm.weapon_attachment[:3, 3].tolist(),
                        'source_face_normal': [0, 0, -1],
                        'native_translation': vm.weapon_bone[1].tolist(),
                        'notes': 'IK aligns the hilt; native palm-to-hilt and finger grip remain unchanged.'},
        'shield_geometry_profile': vm.shield_profile, 'shield_normal': a.shield_normal,
        'clips': vm.provenance, 'max_hand_error': err,
        'groups': vm.groups,
        'notes': 'Derived from the user\'s own game files. Positions copied in DM view space; hit times are integration markers.'}, indent=2)+'\n')


if __name__ == '__main__':
    main()
