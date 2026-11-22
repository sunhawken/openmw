#!/usr/bin/env python3
"""Build the Brutal Morrowind animation and effect data from the two installed games.

    dm-import --dark-messiah <dir> --morrowind <dir> --output <dir>

Reads Dark Messiah VPK/GCF archives or extracted resources and the original Morrowind.bsa, runs every
conversion step in order and writes eight data directories below --output:

    body-data  clinch-data  effects-data  firstperson-data  npc-data  slide-data

Nothing in either game folder is modified. Inputs are the game files plus the
calibration JSON files next to this script; no earlier import is needed.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE
ANIMATION = ROOT / 'scripts/animation'
CREATURES = ROOT / 'scripts/creatures'
EFFECTS = ROOT / 'scripts/effects'
CALIBRATION = HERE / 'calibration'
RECIPE_VERSION = 4  # Misc::DarkMessiahData::sRecipe in the engine must match.
# Conversion steps per import, for progress reporting.
TOTAL_STEPS = 36
KEYFRAMES = ('.kf', '.yaml')
# Steps run side by side, one core each; the matrices involved are 4x4.
ENVIRONMENT = dict(os.environ, OMP_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1', MKL_NUM_THREADS='1')

sys.path.insert(0, str(HERE))
from bsa import Bsa, BsaError  # noqa: E402
sys.path.insert(0, str(ANIMATION))
from dm_sources import source_root  # noqa: E402

MORROWIND_MESHES = (
    'meshes/xbase_anim.nif', 'meshes/xbase_anim.kf',
    'meshes/xbase_anim.1st.nif', 'meshes/xbase_anim.1st.kf',
    'meshes/a/shield_steel.nif',
    'meshes/b/b_n_dark elf_f_hands.1st.nif', 'meshes/b/b_n_dark elf_m_hands.1st.nif',
)
OUTPUTS = ('body-data', 'clinch-data', 'creature-data', 'effects-data', 'firstperson-data', 'kick-data', 'npc-data',
           'slide-data')


class ImportError_(Exception):
    pass


class Cancelled(Exception):
    pass


def find_bsa(path):
    """Accept the game folder or its Data Files folder."""
    for candidate in (path, path / 'Data Files'):
        for name in ('Morrowind.bsa', 'morrowind.bsa'):
            if (candidate / name).is_file():
                return candidate / name
    raise ImportError_(f'Morrowind.bsa not found in {path}')


class Importer:
    def __init__(self, vpks, bsa, output, work):
        self.vpks, self.bsa, self.output, self.work = vpks, bsa, output, work
        self.cache = work / 'source-cache'
        self.meshes = work / 'morrowind/meshes'
        self.stages = work / 'stages'
        self.logs = work / 'logs'
        self.steps = []
        self.timings = []
        self.batches = 0
        self.lock = threading.Lock()
        self.started = time.monotonic()
        self.processes = set()
        self.cancelled = threading.Event()
        self.cancel_reason = None
        self.progress = False
        self.done = 0

    def cancel(self, reason=None):
        """Stop running steps; steps not yet started refuse to start."""
        if reason is not None and not self.cancelled.is_set():
            self.cancel_reason = reason
        self.cancelled.set()
        with self.lock:
            for process in list(self.processes):
                process.kill()

    # ---- plumbing -------------------------------------------------------------------
    def stage(self, name):
        return self.stages / name

    def run(self, label, script, *arguments):
        """Run one conversion script; its output goes to a log shown only on failure."""
        with self.lock:
            if self.cancelled.is_set():
                raise Cancelled()
            self.steps.append(label)
            number = len(self.steps)
            print(f'[{number:2d}] {label}', flush=True)
        log = self.logs / f'{number:02d}-{Path(script).stem}.log'
        command = [sys.executable, str(script), *map(str, arguments)]
        began = time.monotonic()
        with log.open('w') as stream:
            with self.lock:
                if self.cancelled.is_set():
                    raise Cancelled()
                # Not the launcher's input pipe: on Windows, children inheriting it end the
                # importer's own read of it, which looked like a cancel.
                process = subprocess.Popen(command, cwd=ROOT, env=ENVIRONMENT, stdin=subprocess.DEVNULL,
                                           stdout=stream, stderr=subprocess.STDOUT)
                self.processes.add(process)
            process.wait()
            with self.lock:
                self.processes.discard(process)
        if self.cancelled.is_set():
            raise Cancelled()
        self.timings.append((began - self.started, time.monotonic() - self.started, label))
        with self.lock:
            self.done += 1
            if self.progress:
                print(f'PROGRESS {self.done} {TOTAL_STEPS} {label}', flush=True)
        result = process
        if result.returncode:
            tail = '\n'.join(log.read_text(errors='replace').splitlines()[-25:])
            raise ImportError_(f'Step {number} ({label}) failed, full log: {log}\n{tail}')

    def chain(self, label, steps):
        """Run short steps in one interpreter: [(script, argument, ...), ...]."""
        with self.lock:
            self.batches += 1
            listing = self.logs / f'chain-{self.batches}.json'
        listing.write_text(json.dumps([[str(value) for value in step] for step in steps]))
        self.run(f'{label} ({len(steps)} scripts)', HERE / 'run_steps.py', listing)

    def copy(self, source, target):
        shutil.copytree(source, target)
        return target

    @property
    def source(self):
        """Arguments shared by every step that decodes Dark Messiah animation."""
        return ('--vpk-root', self.vpks, '--work-dir', self.cache,
                '--skeleton', self.meshes / 'xbase_anim.1st.nif',
                '--grip-animation', self.meshes / 'xbase_anim.1st.kf')

    @property
    def contract(self):
        return self.stage('body') / 'body-camera-contract.json'

    # ---- steps ----------------------------------------------------------------------
    def prepare(self):
        for folder in (self.cache, self.meshes, self.stages, self.logs):
            folder.mkdir(parents=True, exist_ok=True)
        archive = Bsa(self.bsa)
        missing = [name for name in MORROWIND_MESHES if name not in archive]
        if missing:
            raise ImportError_('Morrowind.bsa lacks original files: ' + ', '.join(missing))
        for name in MORROWIND_MESHES:
            archive.extract(name, self.work / 'morrowind')

    def effects(self):
        out = self.stage('effects')
        for label, script in (('Sounds and particle textures', 'import_dark_messiah_effects.py'),
                              ('Blood decals', 'import_dark_messiah_decals.py'),
                              ('Cut surfaces', 'import_dark_messiah_caps.py'),
                              ('Footstep sounds', 'import_dark_messiah_footsteps.py')):
            self.run(label, EFFECTS / script, '--vpk-root', self.vpks, '--output', out)

    def body(self):
        self.run('Player body', ANIMATION / 'import_body_awareness.py',
                 '--vpk-root', self.vpks, '--work-dir', self.cache,
                 '--skeleton', self.meshes / 'xbase_anim.nif', '--output', self.stage('body'))

    def npc(self):
        self.run('NPC combat', ANIMATION / 'import_npc.py',
                 '--vpk-root', self.vpks, '--work-dir', self.cache,
                 '--skeleton', self.meshes / 'xbase_anim.nif',
                 '--grip-animation', self.meshes / 'xbase_anim.kf', '--output', self.stage('npc'))

    def sword_arms(self):
        """Sword and shield sets in their base layout."""
        out = self.stage('arms-base')
        sword = (*self.source, '--shield-mesh', self.meshes / 'a/shield_steel.nif', '--shield-keep-size',
                 '--camera-contract', self.contract, '--native-eye-height', '--output', out)
        self.run('Sword and shield arms', ANIMATION / 'import_viewmodel.py', *sword, '--untrimmed-bounce-layout')
        self.run('Wall recoil', ANIMATION / 'import_viewmodel.py', *sword, '--bounce-only')
        # The sword clinch clips and their camera curve live in their own data directory.
        clinch = self.stage('clinch-base')
        for relative in ('Animations/brutal_fp/brutal_clinch.kf', 'Animations/brutal_fp/brutal_clinch_lose.kf',
                         'Animations/brutal_fp/brutal_clinch_win.kf', 'scripts/brutal_assets/clinch_head.lua'):
            (clinch / relative).parent.mkdir(parents=True, exist_ok=True)
            shutil.move(out / relative, clinch / relative)
        shutil.rmtree(out / 'scripts')
        return out

    def family(self, family, output=None):
        return (*self.source, '--camera-contract', self.contract, '--output', output or self.stage('arms-' + family),
                '--family', family, '--staff-mount-offset', '0', '--unit-staff-clock')

    def attack_arms(self):
        self.run('Staff attacks', ANIMATION / 'import_weapon_families.py',
                 *self.family('staff', self.stage('arms-staff-attacks')), '--attacks-only')

    def staff_arms(self):
        """Staff set in its base layout, then its mount and attack clock."""
        attacks = self.stage('arms-staff-attacks')
        self.run('Staff arms', ANIMATION / 'import_weapon_families.py', *self.family('staff'))
        self.staff_attacks.result()
        out = self.stage('arms-staff')
        shutil.copytree(attacks, out, dirs_exist_ok=True)
        mounted, clocked = self.stage('staff-mount'), self.stage('staff-clock')
        self.run('Staff mount', ANIMATION / 'staff_grip_mount.py', '--input', out, '--output', mounted, '--offset', '20')
        self.run('Staff attack clock', ANIMATION / 'staff_combat_clock.py', '--input', mounted, '--output', clocked,
                 *self.source, '--camera-contract', self.contract)
        return clocked

    def dagger_arms(self):
        """Dagger set on the hand anatomy; a plain build beside it supplies the family report."""
        out, plain, report = self.stage('arms-dagger'), self.stage('arms-dagger-plain'), self.stage('dagger-report.json')
        anatomy = self.pool.submit(self.run, 'Dagger hand anatomy', ANIMATION / 'import_dagger_anatomy.py',
                                   *self.source, '--camera-contract', self.contract, '--output', out,
                                   '--family-report', report)
        try:
            self.run('Dagger arms', ANIMATION / 'import_weapon_families.py', *self.family('dagger', plain))
            partial = report.with_name(report.name + '.part')
            shutil.copy2(plain / 'weapon-families-import.json', partial)
            partial.replace(report)
        except BaseException:
            # Release the waiting rebuild instead of leaving it to its timeout.
            report.write_text('{}')
            raise
        anatomy.result()
        return out

    def join_arms(self, base, staff, dagger):
        """Put both family sets beside the sword sets; the dagger run wrote the final skeleton."""
        for family, folder in ((staff, 'brutal_fp_staff'), (dagger, 'brutal_fp_dagger')):
            shutil.copytree(family / 'Animations' / folder, base / 'Animations' / folder)
        skeleton = 'meshes/brutal_fp/xbase_anim.1st.nif'
        shutil.copy2(dagger / skeleton, base / skeleton)
        for name in ('staff-grip-mount-import.json', 'staff-combat-clock-import.json',
                     'weapon-families-attacks-import.json'):
            shutil.copy2(staff / name, base / name)
        shutil.copy2(dagger / 'dagger-anatomy-import.json', base / 'dagger-anatomy-import.json')
        merged = json.loads((staff / 'weapon-families-import.json').read_text())
        merged['dagger'] = json.loads((dagger / 'weapon-families-import.json').read_text())['dagger']
        merged['outputs'] = {path.relative_to(base).as_posix(): sha256(path)
                             for path in sorted(base.rglob('*')) if path.suffix in KEYFRAMES}
        merged['outputs'][skeleton] = sha256(base / skeleton)
        (base / 'weapon-families-import.json').write_text(json.dumps(merged, indent=1) + '\n')
        return base

    def early_poses(self):
        """Pose generation that needs only the game files, started long before its use."""
        return [self.pool.submit(self.run, 'Staff grasp poses', ANIMATION / 'apply_staff_native_grasp.py',
                                 '--generate-only', '--generated', self.stage('staff-generated'),
                                 '--staff-mount-offset', '20', '--profile', CALIBRATION / 'staff-native-grasp.json',
                                 *self.source, '--camera-contract', self.contract)]

    def sword_poses(self):
        return [self.pool.submit(self.run, label, ANIMATION / 'import_native_sword_visual.py', '--capture-only',
                                 '--shield-mesh', self.meshes / 'a/shield_steel.nif', '--captured', self.stage(folder + '.poses'), '--folder', folder,
                                 '--profile', CALIBRATION / 'sword-grip.json', *self.source,
                                 '--camera-contract', self.contract)
                for label, folder in (('Sword cycle poses', 'brutal_fp'), ('Shield cycle poses', 'brutal_fp_shield'))]

    def weapon_feedback(self, base):
        """Charged holds, one process per set, joined into the sword run's directory."""
        folders = dict(sword='brutal_fp', shield='brutal_fp_shield', staff='brutal_fp_staff', dagger='brutal_fp_dagger')
        tasks = [self.pool.submit(self.run, f'Charged holds: {family}', ANIMATION / 'import_charged_holds.py',
                                  '--input', base, '--output', self.stage('holds-' + family), '--families', family,
                                  *self.source, '--camera-contract', self.contract, '--source-sword-hits')
                 for family in folders]
        for task in tasks:
            task.result()
        out = self.stage('holds-sword')
        load = lambda family, name: json.loads((self.stage('holds-' + family) / name).read_text())
        manifest, viewmodel = load('sword', 'weapon-families-import.json'), load('sword', 'viewmodel-import.json')
        report = load('sword', 'charged-holds-import.json')
        for family, folder in list(folders.items())[1:]:
            other = self.stage('holds-' + family)
            shutil.copytree(other / 'Animations' / folder, out / 'Animations' / folder, dirs_exist_ok=True)
            report['families'].update(load(family, 'charged-holds-import.json')['families'])
            if family == 'shield':
                viewmodel['groups'][folder] = load(family, 'viewmodel-import.json')['groups'][folder]
            else:
                manifest[family] = load(family, 'weapon-families-import.json')[family]
        for info in report['families'].values():
            manifest['outputs'].update(info['outputs'])
        manifest['charged_holds'] = report
        (out / 'weapon-families-import.json').write_text(json.dumps(manifest, indent=1) + '\n')
        (out / 'viewmodel-import.json').write_text(json.dumps(viewmodel, indent=2) + '\n')
        (out / 'charged-holds-import.json').write_text(json.dumps(report, indent=2) + '\n')
        return out

    def dagger_branch(self, holds):
        grip = self.stage('dagger-grip')
        self.run('Dagger grip', ANIMATION / 'dagger_anatomy_grip_mount.py', '--input', holds, '--output', grip,
                 '--profile', CALIBRATION / 'dagger-grip.json')
        self.skeleton = grip / 'meshes/brutal_fp/xbase_anim.1st.nif'
        frames = self.stage('arm-frames')
        self.run('Arm frames', ANIMATION / 'calibrate_arm_frames.py', '--input', grip, '--output', frames,
                 '--calibration', CALIBRATION / 'arm-frame.json', '--skeleton', self.skeleton)
        wrist = self.stage('dagger-wrist')
        self.run('Dagger wrists', ANIMATION / 'bake_dagger_wrist_profile.py', '--input', frames, '--output', wrist)
        return wrist

    def staff_branch(self, holds):
        out = self.stage('staff-grasp')
        for task in self.staff_poses:
            task.result()
        self.run('Staff grasp', ANIMATION / 'apply_staff_native_grasp.py', '--input', holds, '--output', out,
                 '--profile', CALIBRATION / 'staff-native-grasp.json', '--generated', self.stage('staff-generated'),
                 '--right-rest-calibration', CALIBRATION / 'arm-frame.json',
                 *self.source, '--camera-contract', self.contract)
        return out

    def sword_branch(self, holds):
        source = (*self.source, '--camera-contract', self.contract)
        anatomy = ('--native-rest-hinge', '--anatomical-hand-target', '--reference-hand-frame', '--source-left-fingers')
        profile = ('--profile', CALIBRATION / 'sword-grip.json')
        step = lambda name: self.stage('sword-' + name)
        retreat = json.loads((CALIBRATION / 'sword-left-retreat.json').read_text())['retreat_vector']
        self.chain('Sword arms, grip, idle and wrists', [
            (ANIMATION / 'dagger_anatomy_grip_mount.py', '--input', holds, '--output', step('start'),
             '--profile', CALIBRATION / 'dagger-grip-interim.json'),
            (ANIMATION / 'sword_anatomy_arms.py', '--input', step('start'), '--output', step('arms'), *source, *anatomy),
            (ANIMATION / 'apply_sword_grip_profile.py', '--input', step('arms'), '--output', step('grip'), *profile),
            (ANIMATION / 'sword_anatomy_arms.py', '--input', step('grip'), '--output', step('left'), *source, *anatomy,
             '--left-mcp-anchor', 'actual'),
            (ANIMATION / 'apply_sword_grip_profile.py', '--input', step('left'), '--output', step('grip2'), *profile),
            (ANIMATION / 'retreat_sword_left_arm.py', '--input', step('grip2'), '--output', step('retreat'),
             '--retreat-vector', *map(repr, retreat)),
            (ANIMATION / 'match_sword_idle_anatomy.py', '--input', step('retreat'), '--output', step('idle'), *profile, *source),
            (ANIMATION / 'bake_sword_wrist_profile.py', '--input', step('idle'), '--output', step('wrist')),
            (ANIMATION / 'compensate_sword_grip_orientation.py', '--before-wrist', step('idle'),
             '--input', step('wrist'), '--output', step('blade'))])
        for task in self.cycle_poses.result():
            task.result()
        cycle = lambda label, folder, out: self.run(label, ANIMATION / 'import_native_sword_visual.py',
            '--input', step('blade'), '--output', out, *profile, *source, '--folder', folder,
            '--captured', self.stage(folder + '.poses'))
        shield = self.pool.submit(cycle, 'Shield full cycle', 'brutal_fp_shield', step('cycle-shield'))
        cycle('Sword full cycle', 'brutal_fp', step('cycle'))
        shield.result()
        # Join the two halves: files of the shield set and its rows in both reports.
        out, other = step('cycle'), step('cycle-shield')
        for path in sorted((other / 'Animations/brutal_fp_shield').glob('*.kf')):
            shutil.copy2(path, out / 'Animations/brutal_fp_shield' / path.name)
        report = json.loads((out / 'native-sword-visual-cycle.json').read_text())
        report['files'] += json.loads((other / 'native-sword-visual-cycle.json').read_text())['files']
        manifest = json.loads((out / 'weapon-families-import.json').read_text())
        manifest['outputs'].update({row['path']: row['sha256'] for row in report['files']})
        manifest['native_sword_visual_cycle'] = report
        (out / 'weapon-families-import.json').write_text(json.dumps(manifest, indent=1) + '\n')
        (out / 'native-sword-visual-cycle.json').write_text(json.dumps(report, indent=2) + '\n')
        return out

    def compose(self, name, dagger, staff, sword=None):
        """One set: dagger branch as the base, staff and sword files from their branches."""
        out = self.copy(dagger, self.stage(name))
        merged = json.loads((dagger / 'weapon-families-import.json').read_text())
        parts = [(staff, ('brutal_fp_staff',), 27)] + ([(sword, ('brutal_fp', 'brutal_fp_shield'), 50)] if sword else [])
        rows = []
        for source, folders, expected in parts:
            files = [path for folder in folders for path in sorted((source / 'Animations' / folder).glob('*.kf'))]
            if len(files) != expected:
                raise ImportError_(f'Expected {expected} animation files in {source}, found {len(files)}')
            for path in files:
                relative = path.relative_to(source)
                before = sha256(out / relative)
                shutil.copy2(path, out / relative)
                merged['outputs'][relative.as_posix()] = sha256(out / relative)
                rows.append(dict(file=relative.as_posix(), before_sha256=before, after_sha256=sha256(out / relative)))
        merged['staff'] = json.loads((staff / 'weapon-families-import.json').read_text())['staff']
        for name in ('staff-native-grasp-import.json', 'staff-native-grasp-profile.json'):
            shutil.copy2(staff / name, out / name)
        if sword:
            for name in ('viewmodel-import.json', 'native-sword-visual-cycle.json'):
                shutil.copy2(sword / name, out / name)
            cycle = json.loads((sword / 'weapon-families-import.json').read_text())['native_sword_visual_cycle']
            merged['native_sword_visual_cycle'] = cycle
        merged['visual_weapon_feedback_composition'] = dict(report='visual-weapon-feedback-composition.json')
        (out / 'weapon-families-import.json').write_text(json.dumps(merged, indent=1) + '\n')
        (out / 'visual-weapon-feedback-composition.json').write_text(json.dumps(dict(merged_files=rows), indent=2) + '\n')
        return out

    def staff_chain(self, dagger, staff):
        """Staff recoveries and the staff and dagger wall recoil. None of it reads the sword sets."""
        current = self.compose('staff-chain', dagger, staff.result())
        names = (('stabilize_staff_lateral_recovery.py', 'staff-lateral'),
                 ('append_weapon_wall_recoil.py', 'family-recoil'),
                 ('stabilize_staff_right_lateral.py', 'staff-right'),
                 ('stabilize_staff_calm_return.py', 'staff-calm'),
                 ('stabilize_staff_left_windup.py', 'staff-left'),
                 ('stabilize_staff_preparation.py', 'staff-preparation'),
                 ('stabilize_staff_wall_recoil.py', 'staff-recoil'),
                 ('stabilize_staff_forward_return.py', 'staff-forward'),
                 ('stabilize_staff_neutral.py', 'staff-neutral'))
        steps = []
        for script, name in names:
            steps.append((ANIMATION / script, '--input', current, '--output', self.stage(name)))
            current = self.stage(name)
        final = self.stage('staff-fidelity')
        steps.append((ANIMATION / 'restore_staff_dm_fast.py', '--runtime', current, '--original', current,
                      '--output', final, '--vpk-root', self.vpks, '--work-dir', self.cache,
                      '--camera-contract', self.contract, '--native', self.meshes))
        self.chain('Staff recoveries, wall recoil and fast chain', steps)
        return final

    def finish(self, dagger, staff, sword, chain):
        """Add the ordinary jump to the full set, then take the refined staff and dagger files."""
        full = self.compose('weapon-feedback', dagger, staff.result(), sword.result())
        self.body_done.result()
        out = self.stage('jump')
        self.run('Ordinary jump', ANIMATION / 'import_viewmodel_jump.py', '--input', full, '--output', out,
                 '--body-input', self.stage('body'), '--body-output', self.stage('body-jump'),
                 '--work-dir', self.cache, '--vpk-root', self.vpks)
        refined = chain.result()
        for folder in ('brutal_fp_staff', 'brutal_fp_dagger'):
            shutil.copytree(refined / 'Animations' / folder, out / 'Animations' / folder, dirs_exist_ok=True)
        for path in sorted(refined.glob('*.json')):
            if not (out / path.name).exists():
                shutil.copy2(path, out / path.name)
        manifest = json.loads((out / 'weapon-families-import.json').read_text())
        chained = json.loads((refined / 'weapon-families-import.json').read_text())
        manifest.update({key: value for key, value in chained.items()
                         if key in ('staff', 'dagger') or key not in manifest})
        manifest['outputs'].update({path.relative_to(out).as_posix(): sha256(path)
                                    for path in sorted(out.rglob('*')) if path.suffix in KEYFRAMES})
        (out / 'weapon-families-import.json').write_text(json.dumps(manifest, indent=1) + '\n')
        return out

    def slide(self):
        self.run('Slide', ANIMATION / 'import_slide.py', *self.source,
                 '--shield-mesh', self.meshes / 'a/shield_steel.nif', '--camera-contract', self.contract,
                 '--body', self.stage('body'), '--body-skeleton', self.meshes / 'xbase_anim.nif',
                 '--output', self.stage('slide-base'))

    def slide_and_clinch_frames(self):
        npc = self.stage('npc') / 'Animations/brutal_npc'
        clinch = self.stage('clinch-base') / 'Animations/brutal_npc'
        clinch.mkdir(parents=True)
        for path in sorted(npc.glob('brutal_clinch*')):
            shutil.move(path, clinch / path.name)
        for label, name in (('Slide arm frames', 'slide'), ('Clinch arm frames', 'clinch')):
            self.run(label, ANIMATION / 'calibrate_arm_frames.py', '--input', self.stage(name + '-base'),
                     '--output', self.stage(name), '--calibration', CALIBRATION / 'arm-frame.json',
                     '--skeleton', self.skeleton)

    def creatures(self):
        """Ragdoll rigs for every creature model, from Morrowind's own meshes and plugins."""
        game, scan = self.bsa.parent, self.work / 'creature-scan.json'
        rigs = self.stage('creature') / 'brutal/creature_rigs.txt'
        self.chain('Creature ragdoll rigs', [
            (CREATURES / 'scan_game.py', '--game', game, '--out', scan),
            (CREATURES / 'build_rigs.py', '--game', game, '--scan', scan, '--rigs', rigs,
             '--summary', self.work / 'creature-rigs.json')])

    def plain_kick(self):
        self.run('Plain kick', HERE / 'import_plain_kick.py', '--vpk-root', self.vpks,
                 '--skeleton', self.meshes / 'xbase_anim.nif', '--work-dir', self.work / 'kick',
                 '--output', self.stage('kick'))

    def publish(self, arms):
        """Move finished directories into place and write the readiness marker last."""
        self.output.mkdir(parents=True, exist_ok=True)
        marker = self.output / 'dm-import-ready.json'
        marker.unlink(missing_ok=True)
        for name, source in (('body-data', self.stage('body-jump')), ('clinch-data', self.stage('clinch')),
                             ('creature-data', self.stage('creature')), ('kick-data', self.stage('kick')),
                             ('effects-data', self.stage('effects')), ('firstperson-data', arms),
                             ('npc-data', self.stage('npc')), ('slide-data', self.stage('slide'))):
            target = self.output / name
            if target.exists():
                shutil.rmtree(target)
            shutil.copytree(source, target)
        files = sorted(path for name in OUTPUTS for path in (self.output / name).rglob('*') if path.is_file())
        marker.write_text(json.dumps(dict(recipe=RECIPE_VERSION, files=len(files)), indent=1) + '\n')
        return len(files)

    def build(self):
        """Run the recipe; branches that do not read each other's files run side by side."""
        self.prepare()
        with ThreadPoolExecutor(max_workers=max(4, os.cpu_count() or 4)) as pool:
            try:
                self.pool = pool
                effects, body, npc = pool.submit(self.effects), pool.submit(self.body), pool.submit(self.npc)
                creatures, kick = pool.submit(self.creatures), pool.submit(self.plain_kick)
                # Arm steps need the body step's camera contract, which it writes first.
                while not self.contract.is_file():
                    if body.done():
                        body.result()
                        raise ImportError_('The body step wrote no camera contract')
                    time.sleep(.05)
                self.body_done = body
                slide = pool.submit(lambda: (body.result(), self.slide()))
                self.staff_attacks = pool.submit(self.attack_arms)
                staff, dagger = pool.submit(self.staff_arms), pool.submit(self.dagger_arms)
                self.staff_poses = self.early_poses()
                # Needed late: these start as cores come free, so they do not slow the steps others wait for.
                self.cycle_poses = pool.submit(lambda: (self.staff_attacks.result(), self.sword_poses())[1])
                base = self.sword_arms()
                holds = self.weapon_feedback(self.join_arms(base, staff.result(), dagger.result()))
                staff, sword = pool.submit(self.staff_branch, holds), pool.submit(self.sword_branch, holds)
                dagger = self.dagger_branch(holds)
                chain = pool.submit(self.staff_chain, dagger, staff)
                frames = pool.submit(lambda: (npc.result(), slide.result(), self.slide_and_clinch_frames()))
                arms = self.finish(dagger, staff, sword, chain)
                frames.result(); effects.result(); creatures.result(); kick.result()
                self.run('Footstep keys', ANIMATION / 'add_footstep_keys.py', self.stage('npc'))
            except BaseException:
                # One failed or cancelled step stops the others instead of waiting for them.
                self.cancel()
                raise
        if self.cancelled.is_set():
            raise Cancelled()
        return self.publish(arms)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


KEPT_LOGS = 'import-logs'


def keep_logs(importer, output):
    """Move the step logs of a cancelled import out of the work folder that is removed."""
    try:
        if not any(importer.logs.iterdir()):
            return None
        target = output / KEPT_LOGS
        shutil.rmtree(target, ignore_errors=True)
        shutil.move(str(importer.logs), str(target))
        return target
    except OSError:
        return None


def main():
    parser = argparse.ArgumentParser(prog='dm-import', description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dark-messiah', type=Path, required=True,
                        help='Dark Messiah folder with VPK/GCF archives, or an extracted mm folder')
    parser.add_argument('--morrowind', type=Path, required=True,
                        help='Morrowind folder or its Data Files folder (the one holding Morrowind.bsa)')
    parser.add_argument('--output', type=Path, required=True, help='Folder that receives the eight data directories')
    parser.add_argument('--work', type=Path, help='Scratch folder (default: <output>/.dm-import-work)')
    parser.add_argument('--timings', action='store_true', help='Print when each step started and ended')
    parser.add_argument('--keep-work', action='store_true', help='Keep intermediate stages and logs after success')
    parser.add_argument('--progress', action='store_true',
                        help='Print "PROGRESS <done> <total> <step>" after every finished step')
    parser.add_argument('--cancel-on-stdin-close', action='store_true',
                        help='Cancel the import when standard input closes (used by the launcher)')
    parser.add_argument('--recipe', action='store_true', help='Print the recipe version and exit')
    if '--recipe' in sys.argv[1:]:
        print(RECIPE_VERSION)
        return 0
    arguments = parser.parse_args()
    output = arguments.output.resolve()
    work = (arguments.work or output / '.dm-import-work').resolve()
    try:
        vpks = source_root(arguments.dark_messiah.expanduser().resolve())
        bsa = find_bsa(arguments.morrowind.expanduser().resolve())
        if work.exists():
            shutil.rmtree(work)
        shutil.rmtree(output / KEPT_LOGS, ignore_errors=True)
        importer = Importer(vpks, bsa, output, work)
        importer.progress = arguments.progress
        if arguments.cancel_on_stdin_close:
            def watch():
                # os.read, not sys.stdin: a daemon thread blocked in a buffered read
                # holds its lock and aborts the interpreter at exit.
                # Without a readable input the import simply runs without this
                # signal; the launcher then ends the process itself.
                try:
                    descriptor = sys.stdin.fileno()
                    while os.read(descriptor, 4096):
                        pass
                except (OSError, ValueError):
                    return
                importer.cancel('the launcher closed the importer input')
            threading.Thread(target=watch, daemon=True).start()
        count = importer.build()
    except Cancelled:
        reason = importer.cancel_reason or 'a step was stopped'
        kept = keep_logs(importer, output)
        shutil.rmtree(work, ignore_errors=True)
        print(f'dm-import: cancelled ({reason})', file=sys.stderr)
        if kept is not None:
            print(f'dm-import: step logs kept in {kept}', file=sys.stderr)
        return 3
    except (ImportError_, BsaError, ValueError, OSError) as error:
        print(f'dm-import: {error}', file=sys.stderr)
        return 1
    if arguments.timings:
        for began, ended, label in sorted(importer.timings):
            print(f'{began:6.1f} {ended:6.1f} {ended - began:5.1f}  {label}')
    if not arguments.keep_work:
        shutil.rmtree(work)
    print(f'Done: {count} files in {output} ({time.monotonic() - importer.started:.0f} s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
