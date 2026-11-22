#!/usr/bin/env python3
"""Download the importer's third-party libraries at their pinned commits.

They go to tools/dm-import/third_party/<name>, each with a .commit file that
the import steps check. Run once after cloning, or let the build do it.
"""
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys

HERE = Path(__file__).resolve().parent


def remove_tree(path):
    """shutil.rmtree that also removes read-only files (git objects on Windows)."""
    for parent, folders, files in os.walk(path):
        for name in folders + files:
            os.chmod(os.path.join(parent, name), stat.S_IWRITE | stat.S_IREAD | stat.S_IEXEC)
    shutil.rmtree(path)


def fetch(name, entry, root):
    target = root / name
    stamp = target / '.commit'
    if stamp.is_file() and stamp.read_text().strip() == entry['commit']:
        return False
    if target.exists():
        remove_tree(target)
    target.mkdir(parents=True)
    git = ['git', '-C', str(target)]
    subprocess.run(git + ['init', '--quiet'], check=True)
    subprocess.run(git + ['fetch', '--quiet', '--depth', '1', entry['url'], entry['commit']], check=True)
    subprocess.run(git + ['checkout', '--quiet', 'FETCH_HEAD'], check=True)
    actual = subprocess.check_output(git + ['rev-parse', 'HEAD'], text=True).strip()
    if actual != entry['commit']:
        sys.exit(f'{name}: expected {entry["commit"]}, got {actual}')
    remove_tree(target / '.git')
    stamp.write_text(entry['commit'] + '\n')
    return True


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / 'third_party'
    dependencies = json.loads((HERE / 'dependencies.json').read_text())
    for name, entry in dependencies.items():
        print(f'{name} {entry["commit"][:12]}: {"fetched" if fetch(name, entry, root) else "up to date"}')


if __name__ == '__main__':
    main()
