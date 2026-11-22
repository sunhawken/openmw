#!/usr/bin/env python3
"""Generate the sprint slide's clips from Dark Messiah's own airborne arm poses.

The arms of every equipment set blend from the armed idle through @jump_in, hold
@jump_idle, and return through @jump_out. Native movement owns the physical eye
lowering. The visible body is the authored pose in slide_pose.py. Outputs are
derived from the user's own game files.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from import_viewmodel import Viewmodel
from import_weapon_families import Family, smooth
from slide_pose import Rig, SlidePose, write_kf

BASIC = 'first_animations_basic'
# Marker clock of the slide: in, loop, out.
IN, LOOP, OUT = 1/3, .46666667, .36666667
OFFSETS = (0., IN+.001, IN+.001+LOOP+.001)
END = OFFSETS[2]+OUT
# The body clip is sampled from single-precision source durations: its
# entry is one 120 Hz key longer than the exact third of a second gives.
BODY_IN = 0.3333333432674408
BODY_OFFSETS = (0., BODY_IN+.001, BODY_IN+.001+LOOP+.001)
MARKS = [(0, 'start'), (IN, 'in stop'), (OFFSETS[1], 'loop start'), (OFFSETS[1]+LOOP, 'loop stop'),
         (OFFSETS[2], 'out start'), (END, 'stop')]


def slide_arms(vm, out, kind):
    """kind names the source's jump clips: sword_noshield, sword_shield, staff or daggers."""
    armed = vm.stance_pose()[0]
    clips = [f'@jump_in_{kind}', f'@jump_idle_{kind}', f'@jump_out_{kind}']
    segments, roll_state = [], {}
    for phase, (clip, duration) in enumerate(zip(clips, (IN, LOOP, OUT))):
        natural = vm.duration(BASIC, clip)
        times = np.linspace(0, duration, max(2, round(duration*30)+1))
        poses = []
        for t in times:
            source = t/duration*natural
            vm.source_clip = (BASIC, clip, source)
            dm = vm.dm_world(BASIC, clip, source)
            pose = vm.finish_pose(vm.solve(dm, roll_state), dm)
            if phase == 0:
                pose = vm.blend_pose(armed, pose, smooth(t/duration))
            elif phase == 2:
                pose = vm.blend_pose(pose, armed, smooth(t/duration))
            poses.append(pose)
        segments.append((OFFSETS[phase], times, poses))
        vm.pending.append(dict(model=BASIC, clip=clip, natural=natural, loop=phase == 1, fade_in=0.,
                               body_camera=None, head_camera=None, return_fade=0.))
    vm.write(out, 'brutal_slide', segments, MARKS)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--vpk-root', type=Path, required=True)
    p.add_argument('--work-dir', type=Path, required=True)
    p.add_argument('--skeleton', type=Path, required=True, help='Morrowind meshes/xbase_anim.1st.nif')
    p.add_argument('--grip-animation', type=Path, required=True, help='Morrowind meshes/xbase_anim.1st.kf')
    p.add_argument('--shield-mesh', type=Path, required=True)
    p.add_argument('--camera-contract', type=Path, required=True)
    p.add_argument('--body', type=Path, required=True, help='Directory with Animations/brutal_firstperson/idle1h.kf')
    p.add_argument('--body-skeleton', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    arguments = (a.vpk_root, a.work_dir, a.skeleton, a.grip_animation, -1., a.shield_mesh, True, a.camera_contract, True)
    for shield, folder, kind in ((False, 'brutal_fp', 'sword_noshield'), (True, 'brutal_fp_shield', 'sword_shield')):
        vm = Viewmodel(*arguments)
        vm.shield = shield
        out = a.output/'Animations'/folder; out.mkdir(parents=True, exist_ok=True)
        slide_arms(vm, out, kind)
    for family, kind in (('staff', 'staff'), ('dagger', 'daggers')):
        vm = Family(*arguments)
        vm.grip_path = a.grip_animation
        vm.configure(family)
        out = a.output/'Animations'/vm.config['folder']; out.mkdir(parents=True, exist_ok=True)
        slide_arms(vm, out, kind)
    rig = Rig(a.body_skeleton)
    slide = SlidePose(rig, rig.sample(a.body/'Animations/brutal_firstperson/idle1h.kf'))
    times, poses = [], []
    for phase, duration in enumerate((BODY_IN, LOOP, OUT)):
        for t in np.linspace(0., duration, int(np.ceil(duration*120))+1):
            u = t/duration
            poses.append(slide.sample(u if phase == 0 else 1-u if phase == 2 else 1))
            times.append(BODY_OFFSETS[phase]+t)
    write_kf(a.output/'Animations/brutal_firstperson/brutal_slide.kf', times, poses, MARKS)
    (a.output/'slide-import.json').write_text(json.dumps(dict(
        converter='import_slide.py', markers=MARKS, derived_from_game_files=True,
        arms='first_animations_basic @jump_in/@jump_idle/@jump_out per equipment set',
        body='authored slide_pose.py'), indent=1)+'\n')


if __name__ == '__main__':
    main()
