#!/usr/bin/env python3
"""Fit the physical native dagger hilt through per-pose anatomical grip frames.

Only carrier and item translation tracks change. The source hand/finger poses,
independent grip rotations, commands and gameplay event clocks stay intact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import numpy as np
from audit_dagger_combat_clock import quaternion_at
from retarget_kf import quat_matrix

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'third_party/io_scene_mw/lib'))
from es3 import nif


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def patch(source, output, profiles):
    if output.exists(): raise ValueError('Use a fresh output directory: ' + str(output))
    manifest = json.loads((source / 'weapon-families-import.json').read_text())
    declaration = manifest['dagger']
    if not declaration.get('hand_anatomy') or declaration.get('anatomy_grip_mount'):
        raise ValueError('Expected an unfitted anatomical-frame source')
    if set(profiles) != {'L', 'R'}: raise ValueError('Both physical dagger grips are required')
    for profile in profiles.values():
        for field in ('hilt_point', 'anchor_native_hand' if 'anchor_native_hand' in profile else 'anchor_frame'):
            value = np.asarray(profile[field])
            if value.shape != (3,) or not np.isfinite(value).all(): raise ValueError('Invalid grip vector: ' + field)
    shutil.copytree(source, output)
    stale_report = output / 'dagger-grip-mount-import.json'
    if stale_report.exists(): stale_report.unlink()
    rows = []
    for path in sorted((output / 'Animations/brutal_fp_dagger').glob('*.kf')):
        scene = nif.NiStream(); scene.load(path)
        names = [e.string_data for e in scene.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
        controllers = list(scene.root.controllers)
        keys = next(e for e in scene.root.extra_datas if isinstance(e, nif.NiTextKeyExtraData)).keys
        stop = next(float(time) for time, key in keys if key == path.stem + ': stop')
        changed = set()
        info = declaration['groups']['brutal_fp_dagger'][path.stem]
        for side, profile in profiles.items():
            native_anchor = profile.get('anchor_native_hand')
            anchor = np.asarray(native_anchor if native_anchor is not None else profile['anchor_frame'])
            point = np.asarray(profile['hilt_point'])
            frame_name = declaration['hand_anatomy']['carrier_nodes'][side]
            item_name = 'Weapon Bone' if side == 'R' else 'Brutal Left Weapon'
            frame = controllers[names.index(frame_name)].data
            item = controllers[names.index(item_name)].data
            old_frame_t = frame.translations.keys.copy()
            frame.translations.keys = np.asarray([np.r_[row[0], anchor if native_anchor is not None else row[1:] + quaternion_at(frame.rotations.keys, float(row[0])) @ anchor]
                                                    for row in old_frame_t], np.float32)
            old_item_t = item.translations.keys.copy()
            def original_translation(time):
                return np.array([np.interp(time, old_item_t[:, 0], old_item_t[:, axis]) for axis in (1, 2, 3)])
            translations = []
            for row in item.rotations.keys:
                time = float(row[0]); rotation = quat_matrix(row[1:][[1, 2, 3, 0]])
                if time <= stop + 1e-5:
                    value = ((anchor if native_anchor is not None else original_translation(time) + quaternion_at(frame.rotations.keys, time) @ anchor)
                             - rotation @ point)
                else:
                    primitive = next((p for p in info['dagger_item_primitives'] if p['side'] == side
                                      and p['start'] - 1e-5 <= time <= p['stop'] + 1e-5), None)
                    if primitive is None: raise ValueError('Rotation outside playable/primitive ranges: ' + str(path))
                    value = -rotation @ point
                translations.append(np.r_[time, value])
            item.translations.keys = np.asarray(translations, np.float32)
            changed.update((frame_name, item_name))
        scene.save(path)
        before = nif.NiStream(); before.load(source / path.relative_to(output))
        after = nif.NiStream(); after.load(path)
        preserved = 0
        for name, old, new in zip(names, before.root.controllers, after.root.controllers):
            for channel in ('rotations', 'translations', 'scales'):
                a, b = getattr(old.data, channel).keys, getattr(new.data, channel).keys
                if name in changed and channel == 'translations': continue
                if a.tobytes() != b.tobytes(): raise ValueError('Changed source pose: ' + name + ':' + channel)
                preserved += len(a)
        original_keys = next(e for e in before.root.extra_datas if isinstance(e, nif.NiTextKeyExtraData)).keys
        if list(original_keys) != list(keys): raise ValueError('Changed source/gameplay keys')
        rel = path.relative_to(output).as_posix(); output_sha = sha(path)
        manifest.setdefault('outputs', {})[rel] = output_sha
        rows.append(dict(file=rel, source_sha256=sha(source / rel), output_sha256=output_sha,
                         other_channel_keys_byte_identical=preserved))
    declaration['anatomy_grip_mount'] = profiles
    declaration['hand_anatomy']['right_placement'] = 'physical native-hand hilt anchor'
    declaration['hand_anatomy']['grip_placement'] = 'fixed-native-hand-hilt'
    declaration.pop('right_grip_mount', None)
    (output / 'weapon-families-import.json').write_text(json.dumps(manifest, indent=1) + '\n')
    report = dict(source=str(source.resolve()), output=str(output.resolve()), profiles=profiles,
                  invariant='Native model hilt_point remains at the declared per-pose arm-frame anchor.',
                  recipe_sha256=sha(Path(__file__)), rows=rows)
    (output / 'dagger-anatomy-grip-import.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--profile', type=Path, required=True)
    args = parser.parse_args()
    result = patch(args.input, args.output, json.loads(args.profile.read_text()))
    print(json.dumps(dict(output=result['output'], groups=len(result['rows']), profiles=result['profiles'],
                          preserved_keys=sum(row['other_channel_keys_byte_identical'] for row in result['rows'])), indent=2))
