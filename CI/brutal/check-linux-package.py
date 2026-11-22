#!/usr/bin/env python3
"""Check the Linux package without injecting library or Qt search paths.

Run this in the build image and in each clean runtime image. Every ELF, including
the importer's Python extensions, must load with the runtime image's libraries.
"""
import os
from pathlib import Path
import re
import subprocess
import sys


def version(value):
    return tuple(map(int, value.split('.')))


def main():
    package = Path(sys.argv[1]).resolve()
    environment = dict(os.environ)
    for name in ('LD_LIBRARY_PATH', 'LD_PRELOAD', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH'):
        environment.pop(name, None)
    failures = []
    count = 0
    glibc = set()
    for path in sorted(package.rglob('*')):
        if path.is_symlink():
            if not path.exists() or not path.resolve().is_relative_to(package):
                failures.append(f'{path.relative_to(package)}: symlink leaves the package or is broken')
            continue
        if not path.is_file():
            continue
        with path.open('rb') as stream:
            if stream.read(4) != b'\x7fELF':
                continue
        count += 1
        label = str(path.relative_to(package))
        dynamic = subprocess.check_output(['readelf', '-d', str(path)], text=True)
        direct = set(re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', dynamic))
        versions = subprocess.check_output(['readelf', '-V', str(path)], text=True)
        needs = versions.partition('Version needs section')[2]
        required = set(re.findall(r'Name: GLIBC_([\d.]+)', needs))
        glibc.update(required)
        if any(version(v) > (2, 38) for v in required):
            failures.append(f'{label}: requires glibc {max(required, key=version)} (limit 2.38)')
        if path.parent == package or path.is_relative_to(package / 'lib') or path.is_relative_to(package / 'plugins'):
            relative = os.path.relpath(package / 'lib', path.parent)
            expected = '$ORIGIN' if relative == '.' else '$ORIGIN/' + relative
            paths = re.findall(r'\((?:RUNPATH|RPATH)\).*?\[([^\]]*)\]', dynamic)
            if paths != [expected]:
                failures.append(f'{label}: library path {paths!r}, expected {expected}')
        result = subprocess.run(['ldd', str(path)], env=environment, text=True, capture_output=True)
        output = result.stdout + result.stderr
        for line in output.splitlines():
            if 'not found' in line:
                failures.append(f'{label}: {line.strip()}')
            match = re.match(r'\s*(\S+) => (/.+?)\s+\(0x[0-9a-fA-F]+\)', line)
            if match and match[1] in direct and (package / 'lib' / match[1]).is_file():
                if not Path(match[2]).resolve().is_relative_to(package):
                    failures.append(f'{label}: bundled {match[1]} resolved outside the package: {match[2]}')
        if result.returncode and not ('statically linked' in output or 'not a dynamic executable' in output):
            if 'not found' not in output:
                failures.append(f'{label}: ldd failed: {output.strip()}')
    if not (package / 'qt.conf').is_file():
        failures.append('Missing qt.conf')
    if failures:
        sys.exit('Linux package check failed:\n  ' + '\n  '.join(failures))
    print(f'Checked {count} ELF files; glibc requirement <= {max(glibc, key=version)}; all libraries resolved')


if __name__ == '__main__':
    main()
