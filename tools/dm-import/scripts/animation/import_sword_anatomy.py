#!/usr/bin/env python3
"""Native sword anatomy with explicit hand-role calibration.

Retargets the sword arms around the dominant Hand pivot. Used by the sword arm
steps only; other weapon families and the native controller are unaffected.
"""
import numpy as np

from retarget_kf import cross, norm

from import_viewmodel import Viewmodel, frame, BASIS, to4
from retarget_kf import matrix_quat, quat_matrix


class SwordAnatomy(Viewmodel):
    def __init__(self, *args, **kwargs):
        native_rest_hinge = kwargs.pop('native_rest_hinge', False)
        self.rest_relative_wrist = kwargs.pop('rest_relative_wrist', False)
        self.anatomical_wrist_target = kwargs.pop('anatomical_wrist_target', False)
        self.anatomical_hand_target = kwargs.pop('anatomical_hand_target', False)
        self.reference_hand_frame = kwargs.pop('reference_hand_frame', False)
        self.source_left_fingers = kwargs.pop('source_left_fingers', False)
        self.left_mcp_anchor = kwargs.pop('left_mcp_anchor', None)
        super().__init__(*args, **kwargs)
        self.source_right_map = self.hand_map['R'].copy()
        self.source_weapon_mount = tuple(value.copy() for value in self.weapon_bone)
        knuckles = np.asarray([self.local(f'Bip01 R Finger{i}')[1] for i in (1, 2, 3, 4)])
        self.native_right_palm = frame(knuckles.mean(0), knuckles[3] - knuckles[0])
        if native_rest_hinge:
            rest = self.fk({})
            shoulder, elbow, wrist = [rest[name][:3, 3] for name in
                                      ('Bip01 R UpperArm', 'Bip01 R Forearm', 'Bip01 R Hand')]
            normal = cross(elbow - shoulder, wrist - elbow)
            if normal @ (rest['Bip01 R UpperArm'][:3, :3] @ self.hinge['R']) < 0:
                self.hinge['R'] *= -1
        rest = self.fk({})
        shoulder, elbow, wrist = [rest[name][:3, 3] for name in
                                  ('Bip01 R UpperArm', 'Bip01 R Forearm', 'Bip01 R Hand')]
        normal = cross(elbow - shoulder, wrist - elbow); normal /= norm(normal)
        self.native_upper_basis = frame(self.local('Bip01 R Forearm')[1],
            rest['Bip01 R UpperArm'][:3, :3].T @ normal)
        self.native_fore_basis = frame(self.local('Bip01 R Hand')[1],
            rest['Bip01 R Forearm'][:3, :3].T @ normal)
        source_rest = {}
        bones = self.dm_bones('first_basic')
        for bone in bones:
            local = to4(quat_matrix(bone.quat), np.asarray(bone.position))
            source_rest[bone.name] = source_rest[bones[bone.parent_id].name] @ local if bone.parent_id >= 0 else local
        shoulder, elbow, wrist = [source_rest[name][:3, 3] for name in
                                  ('SK_R_Shoulder', 'SK_R_Elbow', 'SK_R_Wrist')]
        normal = cross(elbow - shoulder, wrist - elbow); normal /= norm(normal)
        self.source_upper_guide = source_rest['SK_R_Shoulder'][:3, :3].T @ normal
        self.source_wrist_guide = source_rest['SK_R_Wrist'][:3, :3].T @ normal
        self.hand_target_calibration = {}
        for side in ('R', 'L'):
            shoulder, elbow, hand = [source_rest[f'SK_{side}_{name}'][:3, 3]
                                     for name in ('Shoulder', 'Elbow', 'Hand')]
            normal = cross(elbow - shoulder, hand - elbow)
            normal /= norm(normal)
            ns, ne, nh = [rest[f'Bip01 {side} {name}'] for name in ('UpperArm', 'Forearm', 'Hand')]
            native_normal = cross(ne[:3, 3] - ns[:3, 3], nh[:3, 3] - ne[:3, 3])
            native_normal /= norm(native_normal)
            knuckles = np.asarray([self.local(f'Bip01 {side} Finger{i}')[1] for i in (1, 2, 3, 4)])
            self.hand_target_calibration[side] = dict(
                palm=frame(knuckles.mean(0), knuckles[3] - knuckles[0]),
                upper_basis=frame(self.local(f'Bip01 {side} Forearm')[1], ns[:3, :3].T @ native_normal),
                fore_basis=frame(self.local(f'Bip01 {side} Hand')[1], ne[:3, :3].T @ native_normal),
                upper_guide=source_rest[f'SK_{side}_Shoulder'][:3, :3].T @ normal,
                distal_guide=source_rest[f'SK_{side}_Wrist'][:3, :3].T @ normal)
        from retarget_anatomical_arm import calibrate_arm_frames
        self.reference_arm_calibration = calibrate_arm_frames(source_rest, rest,
            {name: self.local(name) for name in self.nodes})

    def solve(self, dm, roll_state=None, weapon_grip=True):
        if self.anatomical_hand_target:
            # The source Wrist is a weighted twist helper. Hand is the actual
            # palm flexion pivot, as the first_basic skin shows.
            if self.reference_hand_frame:
                for side, calibration in self.reference_arm_calibration.items():
                    self.hand_map[side] = calibration['hand_map']
            for side, calibration in ([] if self.reference_hand_frame else self.hand_target_calibration.items()):
                names = [f'SK_{side}_Finger_{letter}1' for letter in 'ABCD']
                if names[-1] not in dm and side == 'R':
                    names[-1] = 'SK_Finger_D1'
                points = np.asarray([dm[name][:3, 3] for name in names])
                hand = dm[f'SK_{side}_Hand']
                palm = frame(points.mean(0) - hand[:3, 3], points[3] - points[0])
                self.hand_map[side] = hand[:3, :3].T @ palm @ calibration['palm'].T
            self.weapon_bone = tuple(value.copy() for value in self.source_weapon_mount)
            pose = super().solve(dm, roll_state, weapon_grip)
            pose = self.apply_anatomical_hand_targets(pose, dm)
            if weapon_grip:
                pose['Weapon Bone'] = self.weapon_bone
            return pose
        by_name = {name.casefold(): matrix for name, matrix in dm.items()}
        names = [f'sk_r_finger_{letter}1' for letter in 'abcd']
        if names[-1] not in by_name:
            names[-1] = 'sk_finger_d1'
        points = np.asarray([by_name[name][:3, 3] for name in names])
        wrist, hand = by_name['sk_r_wrist'], by_name['sk_r_hand']
        anatomical = frame(points.mean(0) - wrist[:3, 3], points[3] - points[0])
        mapping = hand[:3, :3].T @ anatomical @ self.native_right_palm.T
        correction = mapping.T @ self.source_right_map
        self.hand_map['R'] = mapping
        old_rotation, old_translation = self.source_weapon_mount
        self.weapon_bone = ((old_rotation, old_translation) if self.anatomical_wrist_target else
                            (correction @ old_rotation, correction @ old_translation))
        pose = super().solve(dm, roll_state, weapon_grip)
        if self.anatomical_wrist_target:
            pose = self.apply_anatomical_wrist_target(pose, dm)
        elif self.rest_relative_wrist:
            pose = self.apply_rest_relative_twist(pose, roll_state)
        if weapon_grip:
            pose['Weapon Bone'] = self.weapon_bone
        return pose

    def apply_anatomical_hand_targets(self, pose, dm):
        """Retarget both source arm directions around the dominant Hand pivot.

        Native lengths and bind skin frames stay intact. Clavicle translations
        absorb the measured anthropometric difference; source Wrist guides the
        distal forearm roll. Items keep rigid native hand-local grip mounts.
        This is a declared proportion/grip adaptation.
        """
        from retarget_anatomical_arm import solve_arm_targets
        calibrations = self.reference_arm_calibration if self.reference_hand_frame else self.hand_target_calibration
        calibrations = {side: dict(values, hand_map=self.hand_map[side]) for side, values in calibrations.items()}
        targets = None
        if self.left_mcp_anchor:
            from retarget_anatomical_arm import mcp_hand_target
            targets = {'L': mcp_hand_target(dm, 'L', self.reference_arm_calibration['L'], BASIS,
                self.to_mw, reference=self.left_mcp_anchor == 'reference')}
        return solve_arm_targets(pose, dm, self.fk(pose),
            {name: self.local(name) for name in self.nodes}, self.parents,
            calibrations, BASIS, self.to_mw, hand_targets=targets)

    def finish_pose(self, pose, dm):
        if not self.source_left_fingers:
            return pose
        from retarget_anatomical_arm import follow_finger_directions
        return follow_finger_directions(pose, dm, self.fk(pose),
            {name: self.local(name) for name in self.nodes}, self.parents, BASIS, 'L')

    def apply_anatomical_wrist_target(self, pose, dm):
        """Retarget toward the Wrist helper target.

        Wrist is a weighted twist helper, not the dominant palm flexion pivot.
        The native mount stays rigid in the hand, so anatomical proportion changes
        become a declared shoulder/hilt adaptation rather than an empty grip.
        """
        upper = BASIS @ (dm['SK_R_Elbow'][:3, 3] - dm['SK_R_Shoulder'][:3, 3])
        fore = BASIS @ (dm['SK_R_Wrist'][:3, 3] - dm['SK_R_Elbow'][:3, 3])
        upper /= norm(upper); fore /= norm(fore)
        wrist = self.to_mw(dm['SK_R_Wrist'][:3, 3])
        elbow = wrist - norm(self.local('Bip01 R Hand')[1]) * fore
        shoulder = elbow - norm(self.local('Bip01 R Forearm')[1]) * upper
        Ru = frame(upper, BASIS @ dm['SK_R_Shoulder'][:3, :3] @ self.source_upper_guide) @ self.native_upper_basis.T
        Rf = frame(fore, BASIS @ dm['SK_R_Wrist'][:3, :3] @ self.source_wrist_guide) @ self.native_fore_basis.T
        Rh = BASIS @ dm['SK_R_Hand'][:3, :3] @ self.hand_map['R']
        current = self.fk(pose)
        neck = current[self.parents['Bip01 R Clavicle']]
        Rc = self.local('Bip01 R Clavicle')[0]
        Rcw = neck[:3, :3] @ Rc
        clavicle = shoulder - Rcw @ self.local('Bip01 R UpperArm')[1]
        result = dict(pose)
        result['Bip01 R Clavicle'] = (Rc, neck[:3, :3].T @ (clavicle - neck[:3, 3]))
        result['Bip01 R UpperArm'] = (Rcw.T @ Ru, self.local('Bip01 R UpperArm')[1])
        result['Bip01 R Forearm'] = (Ru.T @ Rf, self.local('Bip01 R Forearm')[1])
        result['Bip01 R Hand'] = (Rf.T @ Rh, self.local('Bip01 R Hand')[1])
        return result

    def apply_rest_relative_twist(self, pose, roll_state=None):
        """Share physical wrist twist after removing its native bind basis.

        Both the zero-roll forearm and its residual are expressed in forearm
        coordinates. Hand and blade world transforms remain unchanged.
        """
        world = self.fk(pose)
        upper, forearm, hand = [world[name] for name in
                               ('Bip01 R UpperArm', 'Bip01 R Forearm', 'Bip01 R Hand')]
        normal = cross(forearm[:3, 3] - upper[:3, 3], hand[:3, 3] - forearm[:3, 3])
        normal /= norm(normal)
        axis = self.local('Bip01 R Hand')[1].copy(); axis /= norm(axis)
        hinge = self.hinge['R'] - axis * (axis @ self.hinge['R'])
        hinge /= norm(hinge)
        base = frame(hand[:3, 3] - forearm[:3, 3], normal) @ frame(axis, hinge).T
        local = base.T @ hand[:3, :3]
        residual = local @ self.local('Bip01 R Hand')[0].T
        q = matrix_quat(residual)
        twist = float(2 * np.arctan2(q[1:] @ axis, q[0]))
        if roll_state is not None:
            previous = roll_state.get('R_anatomical', twist)
            twist = previous + np.arctan2(np.sin(twist - previous), np.cos(twist - previous))
            roll_state['R_anatomical'] = twist
        angle = twist * .5
        x, y, z = axis
        skew = np.array([[0., -z, y], [z, 0., -x], [-y, x, 0.]])
        rotation = np.eye(3) * np.cos(angle) + (1 - np.cos(angle)) * np.outer(axis, axis) + np.sin(angle) * skew
        target = base @ rotation
        result = dict(pose)
        result['Bip01 R Forearm'] = (upper[:3, :3].T @ target, pose['Bip01 R Forearm'][1])
        result['Bip01 R Hand'] = (target.T @ hand[:3, :3], target.T @ (hand[:3, 3] - forearm[:3, 3]))
        return result
