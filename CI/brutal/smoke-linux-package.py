#!/usr/bin/env python3
"""Launch every package entry point on X11 and Wayland in a clean runtime."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def wait_for(check, process, label):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f'{label} exited with {process.returncode}')
        if check():
            return
        time.sleep(.1)
    raise RuntimeError(f'{label} did not become ready')


def main():
    package = Path(sys.argv[1]).resolve()
    subprocess.run([sys.executable, str(Path(__file__).with_name('check-linux-package.py')), str(package)], check=True)
    with tempfile.TemporaryDirectory(prefix='brutal-runtime-') as directory:
        root = Path(directory)
        env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root / 'config'),
                   XDG_DATA_HOME=str(root / 'data'), XDG_CACHE_HOME=str(root / 'cache'),
                   XDG_RUNTIME_DIR=str(root / 'runtime'), PYTHONDONTWRITEBYTECODE='1', LANG='C.UTF-8')
        for name in ('LD_LIBRARY_PATH', 'LD_PRELOAD', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH',
                     'PYTHONPATH', 'PYTHONHOME', 'DISPLAY', 'WAYLAND_DISPLAY'):
            env.pop(name, None)
        (root / 'runtime').mkdir(mode=0o700)
        config = root / 'config/brutalmorrowind'
        config.mkdir(parents=True)
        (config / 'launcher.cfg').write_text('[General]\nfirstrun=false\n')
        python = package / 'dm-import/python/bin/python3'
        subprocess.run([str(python), '-I', '-B', '-Wignore::DeprecationWarning', '-c',
                        'import ctypes, crypt, ssl, sqlite3; import numpy as np; from PIL import Image; '
                        'assert np.linalg.det(np.eye(4)) == 1; '
                        'assert Image.new("RGB", (2, 2)).size == (2, 2)'],
                       cwd=root, env=env, check=True, timeout=30)
        print('PASS standalone Python, NumPy, Pillow and standard native modules', flush=True)
        for program, argument in [('brutalmorrowind', '--version'), ('brutalmorrowind-iniimporter', '--help'),
                                  ('brutalmorrowind-navmeshtool', '--help'), ('brutalmorrowind-import', '--help')]:
            for suffix in ('', '.x86_64'):
                result = subprocess.run([str(package / (program + suffix)), argument], cwd=root,
                                        env=env, capture_output=True, text=True, timeout=30)
                if result.returncode:
                    raise RuntimeError(f'{program}{suffix}: {result.stdout}\n{result.stderr}')
                print(f'PASS {program}{suffix} {argument}', flush=True)
        xvfb = subprocess.Popen(['Xvfb', ':91', '-screen', '0', '1280x800x24', '-nolisten', 'tcp'],
                                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        weston_log = (root / 'weston.log').open('w')
        weston = subprocess.Popen(['weston', '--backend=headless-backend.so', '--use-pixman',
                                   '--socket=brutal-test', '--idle-time=0', '--no-config'], env=env,
                                  stdout=weston_log, stderr=subprocess.STDOUT)
        try:
            wait_for(lambda: Path('/tmp/.X11-unix/X91').exists(), xvfb, 'Xvfb')
            wait_for(lambda: (root / 'runtime/brutal-test').exists(), weston, 'Weston')
            game_env = dict(env, DISPLAY=':91', SDL_VIDEODRIVER='x11', LIBGL_ALWAYS_SOFTWARE='1')
            missing_content = '__package_check_missing__.omwgame'
            for suffix in ('', '.x86_64'):
                result = subprocess.run([str(package / ('brutalmorrowind' + suffix)), '--no-sound',
                                         '--skip-menu', '--content=' + missing_content],
                                        cwd=root, env=game_env, capture_output=True, text=True, timeout=45)
                output = result.stdout + result.stderr
                # No game files are distributed in CI. Reach the content loader
                # after plugin discovery, graphics, resources and fonts initialize.
                if result.returncode != 1 or f'Failed loading {missing_content}: the content file does not exist' not in output \
                        or 'Missing OSG plugin' in output:
                    raise RuntimeError(f'Game startup did not reach the content loader:\n{output}')
                print(f'PASS game plugins, graphics and resources: brutalmorrowind{suffix}', flush=True)
            for platform in ('xcb', 'wayland'):
                gui_env = dict(env, DISPLAY=':91', WAYLAND_DISPLAY='brutal-test', QT_QPA_PLATFORM=platform,
                               QT_DEBUG_PLUGINS='1', WAYLAND_DEBUG='client')
                for program in ('brutalmorrowind-launcher', 'brutalmorrowind-wizard'):
                    for suffix in ('', '.x86_64'):
                        label = f'{program}{suffix} ({platform})'
                        log = root / (program + suffix + '-' + platform + '.log')
                        with log.open('w') as output:
                            process = subprocess.Popen([str(package / (program + suffix))], cwd=root, env=gui_env,
                                                       stdout=output, stderr=subprocess.STDOUT)
                            try:
                                if platform == 'xcb':
                                    def ready():
                                        return subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(process.pid)],
                                                              env=gui_env, capture_output=True).returncode == 0
                                else:
                                    def ready():
                                        text = log.read_text(errors='replace')
                                        return 'set_title(' in text and 'attach(' in text
                                wait_for(ready, process, label)
                                time.sleep(.5)
                                if process.poll() is not None:
                                    raise RuntimeError(f'{label} closed unexpectedly')
                                text = log.read_text(errors='replace')
                                if 'Cannot load library' in text or 'Could not load the Qt platform plugin' in text:
                                    raise RuntimeError(f'{label} could not load a plugin')
                                print(f'PASS visible window: {label}', flush=True)
                            except Exception:
                                print(log.read_text(errors='replace')[-12000:], file=sys.stderr)
                                raise
                            finally:
                                stop(process)
        finally:
            stop(weston)
            stop(xvfb)
            weston_log.close()
            if weston.returncode not in (0, -15):
                print((root / 'weston.log').read_text(errors='replace'), file=sys.stderr)


if __name__ == '__main__':
    main()
