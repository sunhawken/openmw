"""Pinned third-party commits, as recorded by fetch_dependencies.py."""
from pathlib import Path
import subprocess


def pinned_commit(folder):
    """Commit of a fetched dependency: its .commit file, else its git HEAD."""
    folder = Path(folder)
    stamp = folder / '.commit'
    if stamp.is_file():
        return stamp.read_text().strip()
    return subprocess.check_output(['git', '-C', str(folder), 'rev-parse', 'HEAD'], text=True).strip()
