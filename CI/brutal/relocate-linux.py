#!/usr/bin/env python3
"""Give packaged programs and their plugins paths relative to each ELF file."""
import os
from pathlib import Path
import subprocess
import sys


def main():
    package = Path(sys.argv[1]).resolve()
    files = [*package.glob('*.x86_64'), *(package / 'lib').rglob('*'), *(package / 'plugins').rglob('*')]
    count = 0
    for path in files:
        if not path.is_file() or path.is_symlink():
            continue
        with path.open('rb') as stream:
            if stream.read(4) != b'\x7fELF':
                continue
        relative = os.path.relpath(package / 'lib', path.parent)
        runpath = '$ORIGIN' if relative == '.' else '$ORIGIN/' + relative
        subprocess.run(['patchelf', '--set-rpath', runpath, str(path)], check=True)
        count += 1
    # Python wheels have their own adjacent library directories. Keep those
    # paths and add the standalone runtime's library folder for system fallbacks.
    python = package / 'dm-import/python'
    for path in python.rglob('*'):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open('rb') as stream:
            if stream.read(4) != b'\x7fELF':
                continue
        dynamic = subprocess.check_output(['readelf', '-d', str(path)], text=True)
        current = subprocess.check_output(['patchelf', '--print-rpath', str(path)], text=True).strip()
        relative = os.path.relpath(python / 'lib', path.parent)
        extra = '$ORIGIN' if relative == '.' else '$ORIGIN/' + relative
        paths = current.split(':') if current else []
        if extra not in paths:
            paths.append(extra)
            command = ['patchelf']
            if '(RPATH)' in dynamic:
                command.append('--force-rpath')
            subprocess.run(command + ['--set-rpath', ':'.join(paths), str(path)], check=True)
        count += 1
    print(f'Relocated {count} ELF files')


if __name__ == '__main__':
    main()
