#!/usr/bin/env python3
"""Staff hand reference: a fixed Hand flexion-pivot frame.

Makes the Source Hand role explicit before fitting the native female thumb and
other digits. Released-hand phases use no contact anchor correction.
"""
import numpy as np
from import_weapon_families import Family
from import_viewmodel import Viewmodel, frame, to4, BASIS
from retarget_kf import quat_matrix


class StaffHandReference(Family):
    def prepare_staff(self):
        super().prepare_staff()
        bones = self.dm_bones('first_basic')
        bind = {}
        for bone in bones:
            bind[bone.name] = (bind[bones[bone.parent_id].name] if bone.parent_id >= 0 else np.eye(4)) @ to4(quat_matrix(bone.quat), np.asarray(bone.position))
        hand = bind['SK_L_Hand']
        source_knuckles = np.asarray([bind['SK_L_Finger_' + digit + '1'][:3, 3] for digit in 'ABCD'])
        source_local = (source_knuckles-hand[:3, 3]) @ hand[:3, :3]
        native_local = np.asarray([self.local('Bip01 L Finger' + str(i))[1] for i in (1, 2, 3, 4)])
        self.native_mcp_mean = native_local.mean(0)
        from retarget_anatomical_arm import calibrate_arm_frames
        self.native_local_reference = {name: self.local(name) for name in self.nodes}
        self.arm_reference = calibrate_arm_frames(bind, self.fk({}), self.native_local_reference)
        self.hand_map['L'] = self.arm_reference['L']['hand_map']
        self.staff_grip_fit = dict(palm_delta=(0.,0.,0.), hand_euler=(0.,0.,0.), finger_bends=[(0.,0.,0.)]*5, thumb_root_delta=(0.,0.,0.), position_mode='fixed-Source-Hand-flexion-pivot-plus-native-palm-length')

    def solve(self, dm, roll_state=None, weapon_grip=True):
        # Skip the male whole-palm contact translation in Family.solve.
        pose = Viewmodel.solve(self, dm, roll_state, weapon_grip)
        if not hasattr(self, 'arm_reference'): return pose
        from retarget_anatomical_arm import solve_arm_targets
        world = self.fk(pose)
        return solve_arm_targets(pose, dm, world, self.native_local_reference, self.parents,
            {'L': self.arm_reference['L']}, BASIS, self.to_mw,
            hand_targets={'L': world['Bip01 L Hand'][:3, 3]})

    def hand_target(self, side, dm, hand_rotation, target, weapon_grip):
        if side != 'L' or not hasattr(self, 'native_mcp_mean'): return super().hand_target(side, dm, hand_rotation, target, weapon_grip)
        source_hand = self.to_mw(dm['SK_L_Hand'][:3, 3])
        if not weapon_grip: return source_hand
        weight = self.staff_grip_fit_weights(dm)['palm']
        source_mcp = np.mean([dm['SK_L_Finger_' + digit + '1'][:3, 3] for digit in 'ABCD'], axis=0)
        # The native palm is longer: align the structural MCP row when grasping,
        # while retaining the fixed semantic Hand rotation and its authored motion.
        native_hand = self.to_mw(source_mcp) - hand_rotation @ self.native_mcp_mean
        return source_hand + weight * (native_hand-source_hand)
