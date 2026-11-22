#!/usr/bin/env python3
"""Convert Dark Messiah's staff and dagger viewmodel sets for Morrowind's arms.

Polearms use first_animations_staff, short blades first_animations_daggers. Each set
reuses the one-handed group names and is loaded as its own additional source, as the
shield set is. Dark Messiah's daggers also flip inside the hand between forward and
reverse grip. That motion is keyed on Weapon Bone and on the
second dagger's own mount, Brutal Left Weapon. A staff's left hand follows the actual
Morrowind shaft while the source hand is gripping it.

Outputs are derived from the user's own game files.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from retarget_kf import cross, norm

from import_viewmodel import BASIS, CAMERA_CARRIER, FPS, Viewmodel, frame, to4
from retarget_contract import digit_chains
from retarget_kf import quat_matrix

LEFT_WEAPON = 'Brutal Left Weapon'
FAMILIES = {
    'staff': dict(folder='brutal_fp_staff', model='first_animations_staff', kind='staff',
                  item='items/weapons/staff_combat/staff_combat', right='RightHand_Staff', left=None,
                  idle='anm_staff_idle', walk='@walk_staff', run='@run_staff', sprint='@Megasprint_staff',
                  draw='anm_staff_quickdraw', undraw='anm_staff_undraw', attack='anm_bigslash_staff',
                  kick='anm_staff_kick_', rush='anm_combo_staff_rush', fall=('anm_combo_staff_fall_in',
                  'anm_combo_staff_fall_idle', 'anm_combo_staff_fall_out'), parry='anm_parry_staff',
                  parry_break='anm_state_staff_parry_breaked', jump='staff', versus='anm_staff_versus',
                  # Each source attack has a second strike with a regrip and spin after the first. It plays
                  # unless another attack is clicked from this point on, so a swing never locks for 2.5 s.
                  chain={'_T': 1.0, '_L': .889, '_R': .889, '_B': .933, '': 1.1}),
    'dagger': dict(folder='brutal_fp_dagger', model='first_animations_daggers', kind='daggers',
                   item='items/weapons/daggers/daggers', right='RightHand_Daggers', left='LeftHand_Daggers',
                   idle='anm_daggers_idle', walk='@walk_daggers', run='@run_daggers', sprint='@Megasprint_daggers',
                   draw='anm_daggers_quickdraw', undraw='anm_daggers_undraw', attack='anm_bigslash_Daggers',
                   kick='anm_daggers_kick_', rush='anm_combo_daggers_rush', fall=('anm_combo_daggers_fall_in',
                   'anm_combo_daggers_fall_idle', 'anm_combo_daggers_fall_out'), parry='anm_parry_Daggers',
                   parry_break='anm_state_daggers_parry_breaked', jump='daggers', versus='anm_daggers_versus', chain={}),
}
ITEM_ANIMATIONS = {'staff': 'items/weapons/animations/staff_animations',
                   'dagger': 'items/weapons/animations/daggers_animations'}

# Anatomical retarget calibration for Morrowind's longer palm and shorter thumb base.
# Fitted to the actual skinned bare/steel hands against Morrowind's Silver Staff mesh,
# including triangle interiors, with a finger-arc enclosure constraint. Source movement
# remains animated; the offsets fade out as the source fingers open or leave the shaft.
# Position correction translates the entire left arm. It cannot bend the wrist.
STAFF_GRIP_FIT = dict(
    palm_delta=(2.1709422645156353, 2.9881699622372935, .6314097983769423),
    hand_euler=(0., 0., 0.),
    finger_bends=((.5199999999999999, .4249999999999998, -.36),
                  (-.665, -.615, .06499999999999993),
                  (-.62, -.615, .05500000000000005),
                  (-.705, -.43499999999999994, -.22999999999999995),
                  (-.825, -.635, -.9399999999999997)),
    thumb_root_delta=(-.010230167452344743, -.3254200831123132, -.04869353829479517),
    position_mode='whole_arm_true_wrist')

# Native polearms pivot near their centre, but the source left grip is already
# 36.5 MW units in front of that pivot. Move the item along its shaft so both
# hands hold the haft instead of putting the left fingers beside the spearhead.
# This is an item mount correction: the arm/wrist/finger poses stay intact.
STAFF_MOUNT_OFFSET = 20.


def smooth(x):
    x = float(np.clip(x, 0., 1.))
    return x*x*(3-2*x)


class Family(Viewmodel):
    # Staff data is emitted on the 1.0 source clock and then re-timed in place
    # by staff_combat_clock.py, which keeps section starts.
    unit_staff_clock = False

    def source_rate(self, model, clip):
        if self.unit_staff_clock:
            return 1.
        # Dark Messiah's Combat Staff QCT declares speed_attack=1.10; only fast
        # strikes and charged outs use it;
        # windup, end recovery, parry and locomotion retain their own clocks.
        if getattr(self, 'family', None) == 'staff' and model == 'first_animations_staff':
            if clip in {f'anm_fastattack_staff_{letter}' for letter in 'ABCD'}:
                return 1.1
            if clip.startswith('anm_bigslash_staff') and clip.endswith('_out'):
                return 1.1
        return 1.

    def duration(self, model, name):
        return super().duration(model, name)/self.source_rate(model, name)

    def dm_world(self, model, name, time):
        return super().dm_world(model, name, time*self.source_rate(model, name))

    def segment(self, model, clip, *args, **kwargs):
        rate = self.source_rate(model, clip)
        if (not self.unit_staff_clock and getattr(self, 'family', None) == 'staff'
                and clip.startswith('anm_bigslash_staff') and clip.endswith('_out')):
            # DM enters its idle after the authored out, then crossfades there.
            # A baked idle tail here plus the native idle entry repeats recovery.
            kwargs['return_to'] = None
        result = super().segment(model, clip, *args, **kwargs)
        self.pending[-1].update(source_rate=rate, source_natural=super().duration(model, clip))
        return result

    def configure(self, family):
        self.family, self.config = family, FAMILIES[family]
        c = self.config
        self.weapon_attachment = self.attachment('first_basic', c['right'])
        self.weapon_idle = self.item_idle(c['item'])
        self.grip = self.sample_kf(self.grip_path, 'idle2w: start', lambda n: 'Finger' in n)
        # Item axes inside its attachment: raw -Y is the blade/tip, raw -Z a flat face.
        self.item_frame = frame(self.weapon_idle[:3, :3]@[0, -1, 0], self.weapon_idle[:3, :3]@[0, 0, -1])
        wb = self.weapon_bone[0]
        self.hand_map['R'] = (self.weapon_attachment[:3, :3]@self.item_frame)@frame(wb@[0, 1, 0], wb@[1, 0, 0]).T
        self.to_weapon = frame(np.array([0, 1., 0]), np.array([1., 0, 0]))@self.item_frame.T
        self.left_weapon_attachment = self.attachment('first_basic', c['left']) if c['left'] else None
        self.install_left_mount()
        if self.left_weapon_attachment is not None:
            rotation, _ = self.local(LEFT_WEAPON)
            self.hand_map['L'] = (self.left_weapon_attachment[:3, :3]@self.item_frame)@frame(
                rotation@[0, 1, 0], rotation@[1, 0, 0]).T
        if family == 'staff':
            self.staff_mount_offset = getattr(self, 'staff_mount_offset', STAFF_MOUNT_OFFSET)
            self.prepare_staff()
        self.flips = {}

    # --- second weapon mount -----------------------------------------------------------
    def install_left_mount(self):
        """Mirror Weapon Bone across the hand: fit the reflection to the five finger roots."""
        if LEFT_WEAPON in self.nodes:
            return
        right = np.array([self.nodes[f'Bip01 R Finger{i}'].translation for i in range(5)], float)
        left = np.array([self.nodes[f'Bip01 L Finger{i}'].translation for i in range(5)], float)
        u, _, vt = np.linalg.svd(left.T@right)
        for flip in (np.diag([1., 1, 1]), np.diag([1., 1, -1]), np.diag([1., -1, 1]), np.diag([-1., 1, 1])):
            reflect = u@flip@vt
            if np.linalg.det(reflect) < 0 and norm(reflect@right.T-left.T) < 1.5*norm(left):
                break
        self.mirror_fit = float(norm(reflect@right.T-left.T))
        rotation, translation = self.weapon_bone
        x, y = reflect@rotation@[1, 0, 0], reflect@rotation@[0, 1, 0]
        mount = np.column_stack([x, y, cross(x, y)])
        self.left_mount = (mount, reflect@translation)
        node = self.nif.NiNode(name=LEFT_WEAPON, translation=(reflect@translation).astype(np.float32))
        node.rotation = mount.astype(np.float32)
        self.nodes[LEFT_WEAPON] = node
        self.parents[LEFT_WEAPON] = 'Bip01 L Hand'
        self.keyed = list(dict.fromkeys(self.keyed+[LEFT_WEAPON]))

    def write_skeleton(self, output):
        self.write_camera_skeleton(output)
        path = output/'meshes/brutal_fp/xbase_anim.1st.nif'
        stream = self.nif.NiStream(); stream.load(path)
        hand = next(n for n in stream.objects_of_type(self.nif.NiNode) if n.name == 'Bip01 L Hand')
        if not any(getattr(c, 'name', None) == LEFT_WEAPON for c in hand.children):
            rotation, translation = self.left_mount
            hand.children.append(self.nif.NiNode(name=LEFT_WEAPON, translation=translation.astype(np.float32),
                                                 rotation=rotation.astype(np.float32)))
        stream.save(path)

    # --- animated item inside the hand ---------------------------------------------------
    def item_matrix(self, clip, time):
        model = ITEM_ANIMATIONS[self.family]
        bone = self.dm_bones(model)[0]
        return self.dm_world(model, clip, time)[bone.name][:3, :3]

    def preceding_clip(self, clip):
        """The clip whose final grip this one continues; attacks and guards come in chained parts."""
        if self.family == 'dagger' and (clip == self.config['parry_break'] or
                clip in (self.config['parry']+'_'+v for v in 'ABC')):
            return self.config['parry']+'_idle'
        if self.family == 'dagger' and clip == self.config['versus']+'_r_idle':
            # Weapon_Vs_Weapon starts the idle directly from the guard (no _in clip).
            return self.config['parry']+'_idle'
        names = {a.name for a in self.dm_model(self.config['model'])[3]}
        for later, earlier in (('_out', '_idle'), ('_idle', '_in'), ('_out', '_in')):
            if clip.endswith(later) and clip[:-len(later)]+earlier in names:
                return clip[:-len(later)]+earlier
        if clip == self.config['fall'][1]:
            return self.config['fall'][0]
        if clip == self.config['fall'][2]:
            return self.config['fall'][1]
        return None

    def blade_forward(self, side, clip, time, state):
        """Forward component of the blade direction in that grip."""
        hand, attachment = (('SK_R_Hand', self.weapon_attachment) if side == 'R'
                            else ('SK_L_Hand', self.left_weapon_attachment))
        grip = self.dagger_grip(state)
        world = self.dm_world(self.config['model'], clip, time)[hand]@attachment
        return float((world[:3, :3]@grip@self.item_frame[:3, 0])[0])

    def dagger_grip(self, state):
        forward = self.item_matrix('@right_down_to_up', 1e3)
        return np.eye(3) if state == 'forward' else self.item_matrix('@right_up_to_down', 1e3)@forward.T

    def dagger_plan(self, side, clip):
        """Source item events as (finish time, duration, grip), in authored order.

        The server's viewmodel event dispatcher sends 2119/2120 to the LEFT
        dagger and 2121/2122 to the RIGHT. Option "1" selects the .042 s fast
        item sequence; otherwise its .292 s sequence starts at the event.
        Repeating the current target does not restart a flip.
        """
        key = (side, clip)
        if key in self.flips:
            return self.flips[key]
        model = self.config['model']
        if clip not in {a.name for a in self.dm_model(model)[3]}:
            model = 'first_animations_basic'
        self.sequence_fade(model, clip)
        sequence = self.sequences[model].get(clip)
        events = {2119: ('L', 'reverse'), 2120: ('L', 'forward'),
                  2121: ('R', 'reverse'), 2122: ('R', 'forward')}
        plan = []
        if self.family == 'dagger' and clip == self.config['versus']+'_r_idle':
            # Dark Messiah turns both daggers forward with the fast flip on entry;
            # the clip itself has no item events.
            name = '@'+('left' if side == 'L' else 'right')+'_down_to_up_fast'
            duration = self.duration(ITEM_ANIMATIONS['dagger'], name)
            plan.append((duration, duration, 'forward'))
        for event in sequence.events if sequence else ():
            own, target = events.get(event.event, (None, None))
            if own != side:
                continue
            direction = 'up_to_down' if target == 'reverse' else 'down_to_up'
            item_clip = '@'+('left' if side == 'L' else 'right')+'_'+direction
            if str(event.options).startswith('1'):
                item_clip += '_fast'
            duration = self.duration(ITEM_ANIMATIONS['dagger'], item_clip)
            trigger = float(event.cycle)*self.duration(model, clip)
            plan.append((trigger+duration, duration, target))
        self.flips[key] = plan
        return plan

    def dagger_start(self, side, clip):
        previous = self.preceding_clip(clip)
        if previous is None:
            return 'reverse' if side == 'R' else 'forward'
        plan = self.dagger_plan(side, previous)
        return plan[-1][2] if plan else self.dagger_start(side, previous)

    def dagger_timeline(self, side, clip):
        """Keep the item clock continuous across arm phases, including an unfinished flip."""
        previous = self.preceding_clip(clip)
        before, inherited = (0., []) if previous is None else self.dagger_timeline(side, previous)
        if previous is not None:
            model = self.config['model']
            before += self.duration(model, previous)
            if previous == self.config['parry']+'_idle':
                # A hit/lower/break starts from an established guard, after its
                # one-off item flip, not from the first frame of the arm idle.
                before = max(before, max((finish for finish, _, _ in inherited), default=before))
        return before, inherited+[(before+finish, duration, target)
                                  for finish, duration, target in self.dagger_plan(side, clip)]

    def dagger_state(self, side, clip, time):
        """Rotation in attachment axes, following the actual side and speed of source events."""
        before, plan = self.dagger_timeline(side, clip)
        time += before
        state = 'reverse' if side == 'R' else 'forward'
        active = None
        for finish, duration, target in plan:
            start = finish-duration
            if start > time+1e-8:
                break
            if target == state:
                continue
            state = target
            active = (start, duration, target)
        if active is not None:
            start, duration, target = active
            if time < start+duration:
                name = '@'+('left' if side == 'L' else 'right')+'_'+(
                    'up_to_down' if target == 'reverse' else 'down_to_up')
                if duration < .1:
                    name += '_fast'
                natural = self.duration(ITEM_ANIMATIONS['dagger'], name)
                forward = self.item_matrix('@right_down_to_up', 1e3)
                return self.item_matrix(name, (time-start)/duration*natural)@forward.T
        return self.dagger_grip(state)

    def loop_entry_time(self, model, clip):
        """Source seconds before a looping arm pose has finished its one-off item flips."""
        if self.family != 'dagger':
            return 0.
        latest = 0.
        for side in 'LR':
            before, plan = self.dagger_timeline(side, clip)
            state = 'reverse' if side == 'R' else 'forward'
            for finish, duration, target in plan:
                if target != state:
                    state = target
                    latest = max(latest, finish-before)
        return max(0., latest)

    def finish_pose(self, pose, dm):
        model, clip, time = self.source_clip
        # @-prefixed locomotion and basic clips have no flips: the item stays at rest.
        if self.family == 'dagger':
            for side, node, base in (('R', 'Weapon Bone', self.weapon_bone), ('L', LEFT_WEAPON, self.left_mount)):
                item_time = getattr(self, 'source_item_time', time)
                delta = self.to_weapon@self.dagger_state(side, clip, item_time)@self.to_weapon.T
                pose[node] = (base[0]@delta, base[1])
        else:
            pose[LEFT_WEAPON] = self.left_mount
            rotation, translation = pose.get('Weapon Bone', self.weapon_bone)
            pose['Weapon Bone'] = (rotation, translation+rotation@np.array([0., self.staff_mount_offset, 0.]))
        if dm is not None:
            self.follow_source_fingers(pose, dm)
            if self.family == 'staff':
                self.fit_staff_fingers(pose, dm)
        return pose

    def error(self, dm, overrides, weapon_grip=True):
        # Retarget error compares the original source hand anchor. The declared
        # along-shaft item offset is intentional and does not move that hand.
        if getattr(self, 'family', None) == 'staff' and 'Weapon Bone' in overrides:
            overrides = dict(overrides)
            rotation, translation = overrides['Weapon Bone']
            overrides['Weapon Bone'] = (rotation, translation-rotation@np.array([0., self.staff_mount_offset, 0.]))
        return super().error(dm, overrides, weapon_grip)

    def follow_source_fingers(self, pose, dm):
        """Follow all ten source fingers, including the opening during a regrip.

        Morrowind's two-handed idle holds its left hand almost open, which would read as a palm beside the
        staff or a dagger. Only the phalanx directions are taken: knuckle positions and bone lengths stay
        on the Morrowind hand. The source mixes SK/Sk and omits R from its right little finger's name.
        """
        def unit(v):
            return v/norm(v)
        source = {name.casefold(): transform for name, transform in dm.items()}
        world = self.fk(pose)
        for side in 'LR':
            for digit in digit_chains(side):
                # Exact shared correspondence: four landmarks define three phalanges.
                points = [source[name.casefold()][:3, 3] for name in digit['source']]
                chain = digit['target']
                segments = [BASIS@(points[i+1]-points[i]) for i in range(len(points)-1)]
                parent = world[self.parents[chain[0]]][:3, :3]
                for k, node in enumerate(chain):
                    rotation, translation = pose.get(node, self.local(node))
                    current = parent@rotation
                    along = self.local(chain[k+1])[1] if k+1 < len(chain) else self.local(node)[1]
                    a, d = unit(current@unit(along)), unit(segments[min(k, len(segments)-1)])
                    axis = cross(a, d)
                    s, c = norm(axis), float(a@d)
                    if s > 1e-6:
                        x, y, z = axis/s
                        K = np.array([[0, -z, y], [z, 0, -x], [-y, x, 0]])
                        current = (np.eye(3)+s*K+(1-c)*K@K)@current
                    pose[node] = (parent.T@current, translation)
                    parent = current

    # --- staff left hand ---------------------------------------------------------------
    @staticmethod
    def grip_rotation(axis, angle):
        x, y, z = axis
        k = np.array([[0., -z, y], [z, 0., -x], [-y, x, 0.]])
        return np.eye(3)+np.sin(angle)*k+(1-np.cos(angle))*k@k

    def solve(self, dm, roll_state=None, weapon_grip=True):
        pose = super().solve(dm, roll_state, weapon_grip)
        if self.family != 'staff' or not hasattr(self, 'staff_grip_fit_axes'):
            return pose
        # SK_L_Hand is inside the source palm; SK_L_Wrist is the native Hand anchor.
        # First keep the source arm articulation, then place that whole arm at the
        # shaft. Solving only the wrist toward a contact offset creates an extra bend.
        weight = self.staff_grip_fit_weights(dm)['palm'] if weapon_grip else 0.
        world = self.fk(pose)
        along, _ = self.shaft_distance(dm)
        axis = BASIS@(dm['SK_R_Hand'][:3, :3]@self.weapon_attachment[:3, :3]@self.item_frame[:3, 0])
        palm = self.left_palm+np.asarray(self.staff_grip_fit['palm_delta'])
        target = self.weapon_target(dm['SK_R_Hand'])+axis*self.scale*along-world['Bip01 L Hand'][:3, :3]@palm
        delta = (target-world['Bip01 L Hand'][:3, 3])*weight
        clavicle = 'Bip01 L Clavicle'
        rotation, translation = pose[clavicle]
        pose[clavicle] = rotation, translation+world[self.parents[clavicle]][:3, :3].T@delta
        return pose

    @staticmethod
    def staff_finger_curls(dm):
        source = {name.casefold(): matrix for name, matrix in dm.items()}
        curls = []
        for letter in (None, 'A', 'B', 'C', 'D'):
            base = 'sk_l_thumb_' if letter is None else 'sk_l_finger_'+letter.lower()
            points = [source[base+str(i)][:3, 3] for i in (1, 2, 3, 4)]
            directions = np.diff(points, axis=0)
            directions /= norm(directions, axis=1)[:, None]
            curls.append(float(np.arccos(np.clip(np.sum(directions[:-1]*directions[1:], axis=1), -1., 1.)).sum()))
        return np.asarray(curls)

    def staff_grip_fit_weights(self, dm):
        _, distance = self.shaft_distance(dm)
        palm = 1-smooth((distance-self.dm_grip_distance-1.)/3.)
        closure = np.clip(self.staff_finger_curls(dm)/self.staff_idle_curls, 0., 1.)
        return dict(palm=palm, fingers={i: palm*smooth(value) for i, value in enumerate(closure)})

    def fit_staff_fingers(self, pose, dm):
        weights = self.staff_grip_fit_weights(dm)['fingers']
        for index in range(5):
            chain = (f'Bip01 L Finger{index}', f'Bip01 L Finger{index}1', f'Bip01 L Finger{index}2')
            for node, axis, angle in zip(chain, self.staff_grip_fit_axes[index], self.staff_grip_fit['finger_bends'][index]):
                rotation, translation = pose[node]
                pose[node] = rotation@self.grip_rotation(axis, angle*weights[index]), translation
        node = 'Bip01 L Finger0'
        rotation, translation = pose[node]
        pose[node] = rotation, translation+np.asarray(self.staff_grip_fit['thumb_root_delta'])*weights[0]

    def prepare_staff(self):
        """Measure the native left palm-to-shaft offset from Morrowind's own two-hand idle."""
        stream = self.nif.NiStream(); stream.load(self.grip_path)
        names = [e.string_data for e in stream.root.extra_datas if isinstance(e, self.nif.NiStringExtraData)]
        at = next(float(t) for t, text in stream.root.extra_data.keys
                  if 'idle2w: start' in [x.strip().lower() for x in text.split('\n')])
        overrides = {}
        for name, controller in zip(names, stream.root.controllers):
            if name not in self.nodes or controller.data is None: continue
            rotation, translation = self.local(name)
            keys = controller.data.rotations
            if keys is not None and len(keys.keys):
                keys = keys.keys
                q = np.array([np.interp(at, keys[:, 0], keys[:, i]) for i in range(1, 5)]); q /= norm(q)
                rotation = quat_matrix(q[[1, 2, 3, 0]])
            keys = controller.data.translations
            if keys is not None and len(keys.keys):
                keys = keys.keys
                translation = np.array([np.interp(at, keys[:, 0], keys[:, i]) for i in range(1, 4)])
            overrides[name] = rotation, translation
        world = self.fk(overrides)
        weapon, hand = world['Weapon Bone'], world['Bip01 L Hand']
        axis = weapon[:3, :3]@[0, 1, 0]
        offset = hand[:3, 3]-weapon[:3, 3]
        closest = weapon[:3, 3]+axis*np.dot(offset, axis)
        self.left_palm = hand[:3, :3].T@(closest-hand[:3, 3])
        self.native_hand_spacing = float(np.dot(offset, axis))
        idle = self.dm_world('first_animations_basic', self.config['idle'], 0.)
        self.dm_grip_distance = self.shaft_distance(idle)[1]
        self.staff_idle_curls = np.maximum(self.staff_finger_curls(idle), 1e-6)
        # Calibrate the actual palm plane from its knuckles and physical wrist.
        # A thumb metacarpal is a different anatomical landmark on these two rigs.
        hand, wrist = idle['SK_L_Hand'], idle['SK_L_Wrist']
        source_knuckles = np.asarray([idle['SK_L_Finger_'+letter+'1'][:3, 3] for letter in 'ABCD'])
        source_frame = frame(source_knuckles.mean(0)-wrist[:3, 3], source_knuckles[3]-source_knuckles[0])
        native_knuckles = np.asarray([self.local('Bip01 L Finger'+str(index))[1] for index in (1, 2, 3, 4)])
        native_frame = frame(native_knuckles.mean(0), native_knuckles[3]-native_knuckles[0])
        self.hand_map['L'] = hand[:3, :3].T@source_frame@native_frame.T
        baseline = super().solve(idle, {})
        self.follow_source_fingers(baseline, idle)
        native = self.fk(baseline)
        self.staff_grip_fit_axes = []
        for index in range(5):
            chain = (f'Bip01 L Finger{index}', f'Bip01 L Finger{index}1', f'Bip01 L Finger{index}2')
            points = [native[name][:3, 3] for name in chain]
            normal = cross(points[1]-points[0], points[2]-points[1])
            normal /= norm(normal)
            self.staff_grip_fit_axes.append([native[name][:3, :3].T@normal for name in chain])
        self.staff_grip_fit = STAFF_GRIP_FIT

    def shaft(self, dm):
        handle = dm['SK_R_Hand']@self.weapon_attachment
        origin = handle[:3, 3]
        return origin, handle[:3, :3]@self.item_frame[:3, 0]

    def shaft_distance(self, dm):
        origin, axis = self.shaft(dm)
        offset = dm['SK_L_Hand'][:3, 3]-origin
        along = float(np.dot(offset, axis))
        return along, float(norm(offset-axis*along))

    def hand_target(self, side, dm, hand_rotation, target, weapon_grip):
        if side == 'R':
            return target
        if self.family == 'dagger':
            handle = dm['SK_L_Hand']@self.left_weapon_attachment
            return self.to_mw(handle[:3, 3])-hand_rotation@self.left_mount[1]
        return self.to_mw(dm['SK_L_Wrist'][:3, 3])

    # --- groups ------------------------------------------------------------------------
    def stance_pose(self, armed=True, blocking=False):
        c = self.config
        if blocking:
            model, clip = c['model'], c['parry']+'_idle'
        else:
            model, clip = 'first_animations_basic', c['idle'] if armed else '@idle01'
        self.source_clip = (model, clip, 0.)
        source = self.dm_world(model, clip, 0.)
        if blocking and self.family == 'dagger':
            self.source_item_time = max(self.duration(model, clip), self.loop_entry_time(model, clip))
            try:
                pose = self.finish_pose(self.solve(source, {}, weapon_grip=armed), source)
            finally:
                del self.source_item_time
        else:
            pose = self.finish_pose(self.solve(source, {}, weapon_grip=armed), source)
        return pose, self.sequence_fade(model, clip)

    def weapon(self, out):
        c = self.config
        segments, marks, offset, err = [], [(0, 'start')], 0., 0.
        armed, unarmed = self.stance_pose(), self.stance_pose(armed=False)
        for phase, clip in [('equip', c['draw']), ('unequip', c['undraw'])]:
            times, poses, e = self.segment('first_animations_basic', clip,
                start_pose=unarmed[0] if phase == 'equip' else armed[0],
                return_to=armed if phase == 'equip' else unarmed, runtime_entry=True); err = max(err, e)
            d = times[-1]; segments.append((offset, times, poses))
            attach = self.sequence_event_time('first_animations_basic', clip, 2100 if phase == 'equip' else 2101, d*.4)
            marks += [(offset, phase+' start'), (offset+attach, phase+(' attach' if phase == 'equip' else ' detach')),
                      (offset+d, phase+' stop')]
            offset += d+.05
        # Both sets author _L and _R identically, so there is one lateral slash. The engine
        # gives the staff's sweep (right to left) and the daggers' (left to right) their impulse.
        for attack, suffix in [('chop', '_T'), ('slash', '_L'), ('thrust', '_B'), ('neutral', '')]:
            prefix = c['attack']+suffix
            roll_state = {}
            t1, p1, e1 = self.segment(c['model'], prefix+'_in', roll_state=roll_state, start_pose=armed[0], runtime_entry=True)
            t2, p2, e2 = self.segment(c['model'], prefix+'_out', roll_state=roll_state,
                start_pose=p1[-1], return_to=armed); err = max(err, e1, e2)
            wind, release = t1[-1], t2[-1]
            segments += [(offset, t1, p1), (offset+wind+.001, t2, p2)]
            strike = self.strike_time(prefix+'_out', suffix, release)
            hit = offset+wind+strike; stop = offset+wind+.001+release
            marks += [(offset, attack+' start'), (offset+.15, attack+' min attack'), (offset+wind, attack+' max attack'),
                      (offset+wind+.001, attack+' min hit'), (hit, attack+' hit')]
            for strength in ('small', 'medium', 'large'):
                marks += [(hit+.001, attack+' '+strength+' follow start'), (stop, attack+' '+strength+' follow stop')]
            if suffix in c['chain']:
                marks.append((offset+wind+.001+c['chain'][suffix]/self.source_rate(c['model'], prefix+'_out'), attack+' chain'))
            if self.family in ('dagger', 'staff'):
                # The authored swing's second stroke: the left dagger's, or the staff's own return blow.
                events = self.sequence_events(c['model'], prefix+'_out')
                second = next(t for e, t in events if e == 2139)
                swing = max((t for e, t in events if e == 2107 and t < second), default=second)
                marks += [(offset+wind+swing, attack+' swing2'), (offset+wind+second, attack+' hit2')]
            offset = stop+.05
        model = c['model']
        if self.family == 'dagger':
            # Hit types from first_basic.qct; the visibly striking hand decides " left":
            # measured hand speed at the hit event puts A on the left, B and D on the right,
            # with C and E even, so A C E / B D alternate.
            fast = [(f'fast{v.lower()}', f'anm_fastattack_daggers_{v}', kind, left)
                    for v, kind, left in (('A', 'slash', True), ('B', 'slashright', False), ('C', 'slash', True),
                                          ('D', 'thrust', False), ('E', 'thrust', True))]
            fast += [(f'fastend{v.lower()}', f'anm_fastattack_daggers_end_{v}', None, False) for v in 'ABCD']
            fast += [(f'trans{v.lower()}', f'anm_transition_daggers_{v}', None, False) for v in 'ABCDE']
        else:
            fast = [(f'fast{v.lower()}', f'anm_fastattack_staff_{v}', kind, False)
                    for v, kind in (('A', 'slashright'), ('B', 'thrust'), ('C', 'slash'), ('D', 'thrust'))]
            # Holding through a staff chain plays this end clip, then a fresh windup.
            fast += [(f'fastend{v.lower()}', f'anm_fastattack_staff_{v}_end', None, False) for v in 'ABC']
        offset, e = self.fast_sections(segments, marks, offset, model, armed, fast); err = max(err, e)
        marks.append((offset-.05, 'stop'))
        self.write(out, 'weapononehand', segments, marks)
        return err

    def clinch_set(self, out):
        # Same contest as the sword: the source right idle, its exit, and the parry break on a
        # loss. The camera motion stays the player's head clip used for the sword contest.
        c = self.config
        model, prefix = c['model'], c['versus']
        held = self.stance_pose(blocking=True)
        # DM flips the daggers as a separate item in .042 s; a blended 180 degree turn reads as a
        # crooked pose for the whole .2 s fade.
        self.fade_exempt = {'Weapon Bone', LEFT_WEAPON} if self.family == 'dagger' else set()
        times, poses, error = self.segment(model, prefix+'_r_idle', start_pose=held[0],
            head_camera='@layer_sword_versus_noshield_r_idle')
        final_pose = poses[-1]
        self.write(out, 'brutal_clinch', [(0, times, poses)], [(0, 'start'), (times[-1], 'stop')])
        errors = [error]
        for group, arms, head, duration in [
            ('brutal_clinch_win', prefix+'_r_out', '@layer_sword_versus_noshield_r_out', None),
            ('brutal_clinch_lose', c['parry_break'], '@layer_sword_versus_noshield_out_loose', 40/30)]:
            times, poses, error = self.segment(model, arms, duration=duration, start_pose=final_pose, head_camera=head)
            self.write(out, group, [(0, times, poses)], [(0, 'start'), (times[-1], 'stop')])
            errors.append(error)
        self.fade_exempt = set()
        return max(errors)

    def strike_time(self, clip, suffix, release):
        c = self.config
        strike = self.sequence_event_time(c['model'], clip, 2102, min(.3, release*.4))
        if self.family != 'staff' or suffix != '_L':
            return strike
        # Event 2102 fires with the tip still behind the player, while the swing's trace is
        # already open. A polearm should connect as its head crosses the view.
        def forward(time):
            dm = self.dm_world(c['model'], clip, time)
            return (dm['SK_R_Hand']@self.weapon_attachment)[:3, :3]@self.item_frame[:3, 0]@[1., 0, 0]
        times = np.arange(strike, min(release, strike+.5), 1/120)
        return float(times[int(np.argmax([forward(t) for t in times]))])

    def parry_set(self, out):
        c = self.config
        model, prefix = c['model'], c['parry']
        roll_state = {}
        armed, held = self.stance_pose(), self.stance_pose(blocking=True)
        t1, p1, e1 = self.segment(model, prefix+'_in', roll_state=roll_state, start_pose=armed[0], runtime_entry=True)
        (t2, p2, e2), lead = self.held_loop(model, prefix+'_idle', 1., p1[-1], roll_state)
        t3, p3, e3 = self.segment(model, prefix+'_out', roll_state=roll_state, start_pose=p2[-1], return_to=armed)
        hold = t1[-1]+.001; lower = hold+t2[-1]+.001; end = lower+t3[-1]
        self.write(out, 'brutal_block', [(0, t1, p1), (hold, t2, p2), (lower, t3, p3)],
                   [(0, 'start'), (hold+lead, 'loop start'), (hold+t2[-1], 'loop stop'), (lower, 'lower start'), (end, 'stop')])
        err = max(e1, e2, e3)
        for i, v in enumerate('ABC', 1):
            times, poses, error = self.segment(model, prefix+'_'+v, start_pose=held[0], return_to=held, runtime_entry=True)
            self.write(out, f'brutal_blockhit{i}', [(0, times, poses)], [(0, 'start'), (times[-1], 'stop')])
            err = max(err, error)
        times, poses, error = self.segment(model, c['parry_break'], start_pose=held[0], return_to=armed, runtime_entry=True)
        self.write(out, 'brutal_parrybreak', [(0, times, poses)], [(0, 'start'), (times[-1], 'stop')])
        return max(err, error)

    def rush_set(self, out):
        c = self.config
        armed = self.stance_pose()
        times, poses, err = self.segment(c['model'], c['rush'], start_pose=armed[0], return_to=armed, runtime_entry=True)
        events = sorted(self.sequence_events(c['model'], c['rush']), key=lambda e: e[1])
        swings = [t for e, t in events if e == 2107]
        hits = [t for e, t in events if e == 2102][:1]+[t for e, t in events if e == 2139][:1]
        self.write(out, 'brutal_rush', [(0, times, poses)],
                   [(0, 'start'), (swings[0], 'swing'), (hits[0], 'hit'), (swings[1], 'swing2'), (hits[1], 'hit2'),
                    (times[-1], 'stop')])
        return err

    def fall_set(self, out):
        c = self.config
        jump, idle, strike_clip = c['fall']
        roll_state = {}
        armed = self.stance_pose()
        t1, p1, e1 = self.segment(c['model'], jump, roll_state=roll_state, start_pose=armed[0], runtime_entry=True)
        (t2, p2, e2), lead = self.held_loop(c['model'], idle, .5, p1[-1], roll_state)
        t3, p3, e3 = self.segment(c['model'], strike_clip, roll_state=roll_state, start_pose=p2[-1], return_to=armed)
        events = dict((e, t) for e, t in reversed(self.sequence_events(c['model'], strike_clip)))
        hold = t1[-1]+.001; strike = hold+t2[-1]+.001; end = strike+t3[-1]
        self.write(out, 'brutal_fallattack', [(0, t1, p1), (hold, t2, p2), (strike, t3, p3)],
                   [(0, 'start'), (hold+lead, 'loop start'), (hold+t2[-1], 'loop stop'), (strike, 'out start'),
                    (strike+events.get(2107, 0.), 'swing'), (strike+events[2102], 'hit'), (end, 'stop')])
        return max(e1, e2, e3)

    def build(self, output):
        c = self.config
        out = output/'Animations'/c['folder']; out.mkdir(parents=True, exist_ok=True)
        basic, err = 'first_animations_basic', {}
        err['idle1h'] = self.loop(out, 'idle1h', basic, c['idle'])
        for mode, clip in [('walk', c['walk']), ('run', c['run']), ('sneak', c['walk'])]:
            for direction in ('forward', 'back', 'left', 'right'):
                err[mode+direction] = self.loop(out, mode+direction+'1h', basic, clip)
        err['sprint'] = self.loop(out, 'brutal_sprint1h', basic, c['sprint'])
        err['weapon'] = self.weapon(out)
        for height, group in (('medium', 'brutal_kick1h'), ('low', 'brutal_kick1h_low')):
            armed = self.stance_pose()
            times, poses, e = self.segment(basic, c['kick']+height, start_pose=armed[0], return_to=armed,
                                           body_camera='@attack_kick_'+height, runtime_entry=True)
            self.write(out, group, [(0, times, poses)], [(0, 'start'), (.375, 'hit'), (times[-1], 'stop')])
            err[group] = e
        err['parry'] = self.parry_set(out)
        err['rush'] = self.rush_set(out)
        err['fall'] = self.fall_set(out)
        return err


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--vpk-root', type=Path, required=True)
    p.add_argument('--work-dir', type=Path, required=True)
    p.add_argument('--skeleton', type=Path, required=True, help='Morrowind meshes/xbase_anim.1st.nif')
    p.add_argument('--grip-animation', type=Path, required=True, help='Morrowind meshes/xbase_anim.1st.kf')
    p.add_argument('--camera-contract', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--family', choices=('staff', 'dagger', 'all'), default='all')
    p.add_argument('--attacks-only', action='store_true')
    p.add_argument('--staff-mount-offset', type=float, default=STAFF_MOUNT_OFFSET,
                   help='Native staff mesh offset along Weapon Bone +Y; does not alter either arm')
    p.add_argument('--unit-staff-clock', action='store_true',
                   help='Emit staff attacks on the 1.0 source clock; staff_combat_clock.py re-times them')
    a = p.parse_args()
    Family.unit_staff_clock = a.unit_staff_clock
    a.work_dir.mkdir(parents=True, exist_ok=True)
    manifest = a.output/'weapon-families-import.json'
    # Updating one family must retain the other family's provenance/calibration.
    report = json.loads(manifest.read_text()) if manifest.exists() else {}
    if a.attacks_only:
        # Only weapononehand and the clinch change; every other group stays unchanged.
        extra = {}
        for family in (('staff', 'dagger') if a.family == 'all' else (a.family,)):
            vm = Family(a.vpk_root, a.work_dir, a.skeleton, a.grip_animation, -1., None, True, a.camera_contract, True)
            vm.grip_path = a.grip_animation
            vm.staff_mount_offset = a.staff_mount_offset
            vm.configure(family)
            out = a.output/'Animations'/vm.config['folder']; out.mkdir(parents=True, exist_ok=True)
            extra[family] = dict(weapon=vm.weapon(out), clinch=vm.clinch_set(out), groups=vm.groups, clips=vm.provenance)
            extra[family]['bone_correspondence'] = vm.bone_correspondence
        (a.output/'weapon-families-attacks-import.json').write_text(json.dumps(extra, indent=1, default=str)+'\n')
        return
    for family in (('staff', 'dagger') if a.family == 'all' else (a.family,)):
        vm = Family(a.vpk_root, a.work_dir, a.skeleton, a.grip_animation, -1., None, True, a.camera_contract, True)
        vm.grip_path = a.grip_animation
        vm.staff_mount_offset = a.staff_mount_offset
        vm.configure(family)
        report[family] = dict(max_error=vm.build(a.output), clips=vm.provenance, groups=vm.groups,
                              scale=vm.scale, mirror_fit=vm.mirror_fit,
                              bone_correspondence=vm.bone_correspondence)
        if family == 'staff':
            report[family].update(native_hand_spacing=vm.native_hand_spacing, dm_grip_distance=vm.dm_grip_distance,
                staff_mount_offset=vm.staff_mount_offset,
                staff_grip_fit=vm.staff_grip_fit,
                staff_grip_fit_axes=[[axis.tolist() for axis in chain] for chain in vm.staff_grip_fit_axes],
                staff_idle_curls=vm.staff_idle_curls.tolist(),
                staff_anatomical_map=vm.hand_map['L'].tolist(),
                staff_anatomical_anchor='SK_L_Wrist to mean(A-D knuckles), row D-A')
    vm.write_skeleton(a.output)
    report['converter'] = 'import_weapon_families.py'
    report['notes'] = 'Derived from the user\'s own game files. Hit markers come from source event 2102.'
    report['outputs'] = {q.relative_to(a.output).as_posix(): hashlib.sha256(q.read_bytes()).hexdigest()
                         for q in sorted(a.output.rglob('*')) if q.suffix in ('.kf', '.yaml')}
    (a.output/'weapon-families-import.json').write_text(json.dumps(report, indent=1)+'\n')


if __name__ == '__main__':
    main()
