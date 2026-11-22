#!/usr/bin/env python3
"""Retarget dagger palms from the physical wrist and MCP landmarks.

Source animates Hand relative to Wrist, so one idle calibration cannot describe
its whole cycle. Two inert data nodes carry the compensating grip frame while
the independent item controller continues to play only its raw grip delta.
"""
import argparse
import hashlib
import json
import time
from pathlib import Path

import numpy as np

from import_dagger_combat_feedback import DaggerCombat
from import_viewmodel import frame
from import_weapon_families import LEFT_WEAPON


FRAME_NODES = {'R': 'Brutal Right Dagger Frame', 'L': 'Brutal Left Dagger Frame'}


class DaggerAnatomy(DaggerCombat):
    def configure(self, family):
        if family != 'dagger':
            raise ValueError('DaggerAnatomy only handles daggers')
        super().configure(family)
        self.source_hand_maps = {side: value.copy() for side, value in self.hand_map.items()}
        self.source_mounts = {'R': tuple(value.copy() for value in self.weapon_bone),
                              'L': tuple(value.copy() for value in self.left_mount)}
        self.native_palm_frames = {}
        for side, name in FRAME_NODES.items():
            knuckles = np.asarray([self.local(f'Bip01 {side} Finger{i}')[1] for i in (1, 2, 3, 4)])
            self.native_palm_frames[side] = frame(knuckles.mean(0), knuckles[3] - knuckles[0])
            self.nodes[name] = self.nif.NiNode(name=name)
            self.parents[name] = f'Bip01 {side} Hand'
            self.keyed.append(name)

    def solve(self, dm, roll_state=None, weapon_grip=True):
        by_name = {name.casefold(): matrix for name, matrix in dm.items()}
        frames = {}
        for side in 'LR':
            names = [f'sk_{side.lower()}_finger_{letter}1' for letter in 'abcd']
            if side == 'R' and names[-1] not in by_name:
                names[-1] = 'sk_finger_d1'
            points = np.asarray([by_name[name][:3, 3] for name in names])
            wrist = by_name[f'sk_{side.lower()}_wrist']
            hand = by_name[f'sk_{side.lower()}_hand']
            anatomical = frame(points.mean(0) - wrist[:3, 3], points[3] - points[0])
            mapping = hand[:3, :3].T @ anatomical @ self.native_palm_frames[side].T
            correction = mapping.T @ self.source_hand_maps[side]
            self.hand_map[side] = mapping
            original_rotation, original_translation = self.source_mounts[side]
            mount = correction @ original_rotation, correction @ original_translation
            if side == 'R':
                self.weapon_bone = mount
            else:
                self.left_mount = mount
            frames[FRAME_NODES[side]] = mount
        pose = super().solve(dm, roll_state, weapon_grip)
        pose.update(frames)
        return pose

    def write_skeleton(self, output):
        # The new nodes have no children or skin weights. Other families do not
        # use their values, and Weapon Bone retains its original parent.
        current = self.left_mount
        self.left_mount = self.source_mounts['L']
        try:
            super().write_skeleton(output)
        finally:
            self.left_mount = current
        path = output / 'meshes/brutal_fp/xbase_anim.1st.nif'
        stream = self.nif.NiStream(); stream.load(path)
        nodes = {node.name: node for node in stream.objects_of_type(self.nif.NiNode)}
        for side, name in FRAME_NODES.items():
            hand = nodes[f'Bip01 {side} Hand']
            node = nodes.get(name) or self.nif.NiNode(name=name)
            node.rotation = np.eye(3, dtype=np.float32)
            node.translation = np.zeros(3, dtype=np.float32)
            node.scale = 1.
            # The data frame must be updated before the item sibling consumes it.
            hand.children = [node] + [child for child in hand.children if getattr(child, 'name', None) != name]
        stream.save(path)

    def annotate_items(self, output, manifest):
        saved = self.weapon_bone, self.left_mount
        self.weapon_bone = np.eye(3), np.zeros(3)
        self.left_mount = np.eye(3), np.zeros(3)
        try:
            super().annotate_items(output, manifest)
        finally:
            self.weapon_bone, self.left_mount = saved
        for group, info in manifest['dagger']['groups']['brutal_fp_dagger'].items():
            path = output / 'Animations/brutal_fp_dagger' / (group + '.kf')
            stream = self.nif.NiStream(); stream.load(path)
            names = [entry.string_data for entry in stream.root.extra_datas
                     if isinstance(entry, self.nif.NiStringExtraData)]
            controllers = list(stream.root.controllers)
            textkeys = next(entry for entry in stream.root.extra_datas
                            if isinstance(entry, self.nif.NiTextKeyExtraData))
            marks = [(float(time), str(label)) for time, label in textkeys.keys
                     if str(label) != group + ': dagger arm frame']
            marks.append((0., group + ': dagger arm frame'))
            textkeys.keys = np.asarray(sorted(marks, key=lambda row: row[0]), dtype='<f,O')
            stop = max(time for time, label in marks if label == group + ': stop')
            for side, node in [('R', 'Weapon Bone'), ('L', LEFT_WEAPON)]:
                controller = controllers[names.index(node)]
                old = controller.data.translations.keys
                rows = [row.copy() for row in old if row[0] <= stop + 1e-5]
                for primitive in info['dagger_item_primitives']:
                    if primitive['side'] != side:
                        continue
                    rows.extend([np.array([primitive['start'], 0., 0., 0.]),
                                 np.array([primitive['stop'], 0., 0., 0.])])
                    primitive['basis'] = 'mount-local'
                controller.data.translations.keys = np.asarray(sorted(rows, key=lambda row: row[0]), np.float32)
            stream.save(path)
            info['dagger_arm_frame'] = FRAME_NODES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('vpk-root', 'work-dir', 'skeleton', 'grip-animation', 'camera-contract', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--family-report', type=Path,
                        help='weapon-families-import.json of a dagger build running side by side; awaited after the rebuild')
    args = parser.parse_args()
    vm = DaggerAnatomy(args.vpk_root, args.work_dir, args.skeleton, args.grip_animation,
                       camera_contract=args.camera_contract, native_eye_height=True)
    vm.grip_path = args.grip_animation; vm.configure('dagger')
    errors = vm.build(args.output)
    folder = args.output / 'Animations/brutal_fp_dagger'
    errors['clinch'] = vm.clinch_set(folder)
    vm.write_skeleton(args.output)
    manifest_path = args.output / 'weapon-families-import.json'
    if args.family_report:
        # The caller moves the finished report into place in one step.
        deadline = time.monotonic() + 900
        while not args.family_report.is_file():
            if time.monotonic() > deadline:
                raise SystemExit(f'No dagger report appeared at {args.family_report}')
            time.sleep(.1)
    manifest = json.loads((args.family_report or manifest_path).read_text())
    manifest['dagger']['groups']['brutal_fp_dagger'] = vm.groups['brutal_fp_dagger']
    # Placement measured for another palm basis is never reused.
    manifest['dagger'].pop('right_grip_mount', None)
    manifest['dagger']['hand_anatomy'] = dict(
        converter=Path(__file__).name, mode='pose-wrist-mcp-frame', carrier_nodes=FRAME_NODES,
        frame_composition='bone_local = arm_frame * item_delta',
        source_landmarks='SK_*_Wrist to mean A-D MCP, lateral D-A',
        native_landmarks='Bip01 * Hand to mean Finger1-4 roots, lateral 4-1',
        primitive_mount_bases={side: dict(rotation=mount[0].tolist(), translation=mount[1].tolist())
                               for side, mount in vm.source_mounts.items()},
        native_finger_translations='unchanged', right_placement='unfitted')
    vm.annotate_items(args.output, manifest)
    outputs = {path.relative_to(args.output).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
               for path in sorted(folder.iterdir()) if path.suffix in ('.kf', '.yaml')}
    skeleton = args.output / 'meshes/brutal_fp/xbase_anim.1st.nif'
    outputs[skeleton.relative_to(args.output).as_posix()] = hashlib.sha256(skeleton.read_bytes()).hexdigest()
    manifest.setdefault('outputs', {}).update(outputs)
    manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
    report = dict(converter=Path(__file__).name, factory='import_dagger_anatomy:DaggerAnatomy',
                  errors=errors, frames=FRAME_NODES, outputs=outputs)
    (args.output / 'dagger-anatomy-import.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
