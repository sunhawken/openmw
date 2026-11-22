#!/usr/bin/env python3
"""Build the Python runtime that ships with brutalmorrowind-import.

    package_python.py TARGET [--platform linux-x86_64|windows-x86_64] [--cache DIR]

Downloads the pinned standalone CPython (python.json), checks its SHA-256,
unpacks it to TARGET, installs requirements.txt from binary wheels only,
removes what the importer never uses and precompiles the bytecode, since the
installed program folder is read-only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent
UNUSED = ('test', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'lib2to3', 'pydoc_data')


def default_platform():
    return 'windows-x86_64' if sys.platform == 'win32' else 'linux-x86_64'


def fetch(entry, cache):
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / entry['url'].rsplit('/', 1)[1].replace('%2B', '+')
    if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != entry['sha256']:
        print(f'Downloading {entry["url"]}', flush=True)
        with urllib.request.urlopen(entry['url']) as response, archive.open('wb') as stream:
            shutil.copyfileobj(response, stream)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != entry['sha256']:
        sys.exit(f'{archive.name}: SHA-256 {digest}, expected {entry["sha256"]}')
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('target', type=Path)
    parser.add_argument('--platform', default=default_platform(), choices=('linux-x86_64', 'windows-x86_64'))
    parser.add_argument('--cache', type=Path, default=HERE / '.cache')
    args = parser.parse_args()
    entry = json.loads((HERE / 'python.json').read_text())[args.platform]
    archive = fetch(entry, args.cache)
    target = args.target.resolve()
    if target.exists():
        shutil.rmtree(target)
    unpack = target.parent / (target.name + '.unpack')
    if unpack.exists():
        shutil.rmtree(unpack)
    with tarfile.open(archive) as tar:
        # share/ (terminfo, man pages) is not needed; its hard links also trip
        # the extraction filter of older Python 3.10 releases.
        members = [m for m in tar.getmembers() if not m.name.startswith('python/share/')]
        tar.extractall(unpack, members=members, filter='data')
    (unpack / 'python').rename(target)
    unpack.rmdir()
    windows = args.platform.startswith('windows')
    python = target / ('python.exe' if windows else 'bin/python3')
    if windows != (sys.platform == 'win32'):
        sys.exit('Build the Python runtime on the platform it is for')
    subprocess.run([str(python), '-m', 'pip', 'install', '--no-cache-dir', '--disable-pip-version-check',
                    '--only-binary', ':all:', '--no-warn-script-location', '-r', str(HERE / 'requirements.txt')],
                   check=True)
    stdlib = target / 'Lib' if windows else next((target / 'lib').glob('python3.*'))
    for name in UNUSED:
        shutil.rmtree(stdlib / name, ignore_errors=True)
    subprocess.run([str(python), '-m', 'pip', 'uninstall', '--yes', '--quiet', 'pip'], check=True)
    for folder in ('include', 'share'):
        shutil.rmtree(target / folder, ignore_errors=True)
    if not windows:
        # Tcl/Tk serves only tkinter. (python.json pins the stripped CPython build:
        # the full one carries about 180 MiB of debug symbols.)
        unused = {path for pattern in ('*tcl*', '*tk*', 'itcl*', 'thread*') for path in (target / 'lib').glob(pattern)}
        unused |= set((stdlib / 'lib-dynload').glob('_tkinter*'))
        # Build copies resolve symbolic links, so keep exactly one interpreter
        # (bin/python3, what brutalmorrowind-import runs) and one libpython.
        real = (target / 'bin' / 'python3').resolve()
        for path in list((target / 'bin').iterdir()) + list((target / 'lib').glob('libpython*')):
            if path.is_symlink() or (path.parent.name == 'bin' and path != real):
                unused.add(path)
        unused.discard(real)
        for path in sorted(unused):
            shutil.rmtree(path) if path.is_dir() and not path.is_symlink() else path.unlink(missing_ok=True)
        if real.name != 'python3':
            real.rename(target / 'bin' / 'python3')
    subprocess.run([str(python), '-m', 'compileall', '-q', '-j', '0', '--invalidation-mode', 'unchecked-hash',
                    str(stdlib)], check=True)
    size = sum(p.stat().st_size for p in target.rglob('*') if p.is_file())
    print(f'Python runtime ready: {target} ({size / 2**20:.0f} MiB)')


if __name__ == '__main__':
    main()
