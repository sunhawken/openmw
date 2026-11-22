#!/usr/bin/env python3
"""Apply a measured native left power grip over the source's grasp/open roles.

The arm uses the fixed anatomical Hand reference. The contact calibration is a
native mesh adaptation: four fingers/palm contain the shaft; the thumb closes
against the outer index surface. Source clocks and release gestures stay separate.
"""
import json
from pathlib import Path
import numpy as np

from retarget_kf import cross, norm
from staff_hand_reference import StaffHandReference
from import_viewmodel import BASIS
from retarget_anatomical_arm import solve_arm_targets
from retarget_contract import CONTRACT
from import_weapon_families import smooth


def swing(first, second):
    first = first / norm(first); second = second / norm(second)
    turn = cross(first, second); sine = norm(turn); cosine = float(first @ second)
    if sine < 1e-9:
        if cosine < 0: raise ValueError('Antipodal native grip-axis adaptation')
        return np.eye(3)
    x, y, z = turn/sine
    K = np.array([[0., -z, y], [z, 0., -x], [-y, x, 0.]])
    return np.eye(3) + sine*K + (1-cosine)*K@K


class StaffNativeGrasp(StaffHandReference):
    def __init__(self, *args, **kwargs):
        profile = kwargs.pop('grasp_profile')
        self.grasp_profile_path = Path(profile)
        self.native_grasp = None
        self.native_grasp_enabled = False
        super().__init__(*args, **kwargs)

    def configure(self, family):
        super().configure(family)
        declaration = json.loads(self.grasp_profile_path.read_text())
        if declaration['mode'] != 'native-left-power-grip': raise ValueError('Unexpected native grip profile')
        expected = {name for digit in CONTRACT['sides']['L']['digits'] for name in digit['target']}
        if set(declaration['closed_finger_local_rotations']) != expected: raise ValueError('Incomplete physical native grip')
        for value in declaration['closed_finger_local_rotations'].values():
            R = np.asarray(value)
            if R.shape != (3,3) or not np.isfinite(R).all() or abs(np.linalg.det(R)-1) > 1e-4:
                raise ValueError('Invalid native grip rotation')
        self.native_grasp = declaration
        # Calibrate the native power grip once, in the actual owned idle. The
        # fast clips keep this same hand-local map through every spin/regrip.
        ref = declaration['source_reference']
        dm = self.dm_world(ref['model'], ref['clip'], ref['time'])
        hand = BASIS @ dm['SK_L_Hand'][:3,:3] @ self.hand_map['L']
        axis = BASIS @ self.shaft(dm)[1]
        reference = hand @ np.asarray(declaration['shaft_axis_native_hand_reference'])
        self.quick_grasp_hand_map = self.hand_map['L'] @ hand.T @ swing(reference, axis) @ hand
        self.quick_empty_fist = self.sample_kf(self.grip_path, 'idlehh: start',
                                              lambda name: name.startswith('Bip01 L Finger'))

    def solve(self, dm, roll_state=None, weapon_grip=True):
        self.native_grasp_enabled = weapon_grip
        pose = super().solve(dm, roll_state, weapon_grip)
        if self.native_grasp is None or not weapon_grip: return pose
        if self.source_faithful_quick_grasp():
            return self.solve_quick_grasp(pose, dm)
        weight = self.staff_grip_fit_weights(dm)['palm']
        if weight <= 0.: return pose
        world = self.fk(pose); H = world['Bip01 L Hand']; W = world['Weapon Bone']
        axis = W[:3,1] / norm(W[:3,1])
        reference_axis = np.asarray(self.native_grasp['shaft_axis_native_hand_reference'])
        adapted = swing(H[:3,:3] @ reference_axis, axis) @ H[:3,:3]
        Rh = self.blend_pose({'h':(H[:3,:3], np.zeros(3))}, {'h':(adapted, np.zeros(3))}, weight)['h'][0]
        anchor = np.asarray(self.native_grasp['shaft_anchor_native_hand'])
        nominal = H[:3,3] + Rh @ anchor
        projection = W[:3,3] + axis * np.dot(nominal-W[:3,3], axis)
        goal = projection - Rh @ anchor - axis*self.native_grasp['along_shift_native_units']
        goal = H[:3,3] + weight*(goal-H[:3,3])
        pose = solve_arm_targets(pose, dm, world, self.native_local_reference, self.parents,
            {'L':self.arm_reference['L']}, BASIS, self.to_mw, hand_targets={'L':goal})
        updated = self.fk(pose)
        pose['Bip01 L Hand'] = (updated['Bip01 L Forearm'][:3,:3].T @ Rh,
                               self.native_local_reference['Bip01 L Hand'][1])
        return pose

    def solve_quick_grasp(self, pose, dm):
        """Retain source rotation exactly; adapt native proportions/contact only."""
        world = self.fk(pose)
        Rh = BASIS @ dm['SK_L_Hand'][:3,:3] @ self.quick_grasp_hand_map
        weight = self.staff_grip_fit_weights(dm)['palm']
        source_mcp = np.mean([dm['SK_L_Finger_'+digit+'1'][:3,3] for digit in 'ABCD'],axis=0)
        # Use the direct anatomical target, before the generic right-arm IK's
        # reach clamp. The native left chain preserves source articulation.
        palm_target = self.to_mw(source_mcp) - Rh @ self.native_mcp_mean
        # Proportion correction is anatomical, not a contact state: the native
        # palm stays longer even after release. Fading this MCP alignment out
        # moved the fist/forearm around the shaft whenever the grip gate changed.
        # Keep the authored physical MCP locus through free and grasped phases.
        target = palm_target
        W = world['Weapon Bone']; axis = W[:3,1] / norm(W[:3,1])
        anchor = np.asarray(self.native_grasp['shaft_anchor_native_hand'])
        nominal = target + Rh @ anchor
        projection = W[:3,3] + axis * np.dot(nominal-W[:3,3],axis)
        # The very small calibrated axial shift belongs to the native palm,
        # not the arbitrary +Y sign of the spinning weapon's shaft.
        hand_axis = Rh @ np.asarray(self.native_grasp['shaft_axis_native_hand_reference'])
        along = hand_axis * self.native_grasp['along_shift_native_units']
        target += weight * (projection - Rh @ anchor - along - target)
        pose = solve_arm_targets(pose, dm, world, self.native_local_reference, self.parents,
            {'L':self.arm_reference['L']}, BASIS, self.to_mw, hand_targets={'L':target})
        updated = self.fk(pose)
        pose['Bip01 L Hand'] = (updated['Bip01 L Forearm'][:3,:3].T @ Rh,
                               self.native_local_reference['Bip01 L Hand'][1])
        return pose

    def source_faithful_quick_grasp(self):
        source = getattr(self, 'source_clip', ('', '', 0.))
        return source[0] == 'first_animations_staff' and source[1] in {
            'anm_fastattack_staff_'+suffix for suffix in ('B','C','D','B_end','C_end')}

    def staff_grip_fit_weights(self, dm):
        weights = super().staff_grip_fit_weights(dm)
        if self.native_grasp is None or not self.source_faithful_quick_grasp():
            return weights
        # A closed source fist is still closed while it is away from the staff.
        # Coupling its native finger fit to Hand-pivot/shaft proximity creates
        # spurious open/close attempts (particularly C_end). Keep source closure
        # independent from the physical contact correction.
        curls = self.staff_finger_curls(dm) / self.staff_idle_curls
        fingers={i:smooth(float(value)) for i,value in enumerate(curls)}
        closure = smooth((float(np.mean(curls[1:3])) - .35) / .5)
        if closure==0.:return dict(palm=0.,fingers=fingers)
        hand = BASIS @ dm['SK_L_Hand'][:3,:3] @ self.quick_grasp_hand_map
        axis = BASIS @ self.shaft(dm)[1]
        reference = hand @ np.asarray(self.native_grasp['shaft_axis_native_hand_reference'])
        alignment = smooth(abs(float(reference @ axis)) / .5)
        # The shaft must enter the source curled-finger volume, not merely pass
        # near its Hand pivot. In C/end-C that pivot crosses the shaft several
        # times while the actual fingers remain beside it. Projecting the four
        # MCP/PIP/DIP arcs onto the shaft's normal plane gives an unoriented,
        # pose-local enclosure test. A one-source-unit margin fades the fit as
        # the finger volume approaches; no authored time interval is rewritten.
        origin,source_axis=self.shaft(dm)
        normal=cross(source_axis,[1.,0,0] if abs(source_axis[0])<.9 else [0,1.,0])
        normal/=norm(normal)
        plane=np.column_stack((normal,cross(source_axis,normal)))
        gaps=[]
        for digit in 'ABCD':
            points=np.array([dm['SK_L_Finger_'+digit+str(i)][:3,3]-origin for i in (1,2,3)])@plane
            a=2*(points[1:]-points[0]);b=np.sum(points[1:]**2,axis=1)-np.sum(points[0]**2)
            if abs(np.linalg.det(a))<1e-8:continue
            centre=np.linalg.solve(a,b);radius=norm(points[0]-centre)
            gaps.append(float(norm(centre)-radius))
        enclosure=1-smooth(float(np.median(gaps))) if len(gaps)>=3 else 0.
        return dict(palm=closure*alignment*enclosure,fingers=fingers)

    def fit_staff_fingers(self, pose, dm):
        # Do not apply the male palm/finger bend fit.
        if self.native_grasp is None or not self.native_grasp_enabled: return
        weights = self.staff_grip_fit_weights(dm)['fingers']
        for digit in CONTRACT['sides']['L']['digits']:
            index = digit['index'] if 'index' in digit else next(i for i in range(5) if digit['target'][0] == f'Bip01 L Finger{i}')
            weight = weights[index]
            if weight <= 0.: continue
            for node in digit['target']:
                rotation, translation = pose[node]
                target = np.asarray(self.native_grasp['closed_finger_local_rotations'][node])
                if self.source_faithful_quick_grasp():
                    # A source closed empty fist is not the wider native power
                    # grip around a shaft. Preserve that role while approaching
                    # the staff; open to the physical grip only at real enclosure.
                    target = self.blend_pose({'f':(self.quick_empty_fist[node],translation)},
                        {'f':(target,translation)}, self.staff_grip_fit_weights(dm)['palm'])['f'][0]
                pose[node] = self.blend_pose({'f':(rotation,translation)}, {'f':(target,translation)}, weight)['f']
