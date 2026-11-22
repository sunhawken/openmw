#!/usr/bin/env python3
"""Run several conversion scripts one after another in a single interpreter.

Short steps spend most of their time starting Python and importing NumPy and
the shared modules. A chain of them shares one start-up this way. Each script
still runs as its own `__main__` with its own arguments.

    run_steps.py steps.json     # [[script, argument, ...], ...]
"""
import json
from pathlib import Path
import runpy
import sys


def main():
    steps = json.loads(Path(sys.argv[1]).read_text())
    for script, *arguments in steps:
        print(f'== {Path(script).name}', flush=True)
        folder = str(Path(script).resolve().parent)
        if folder not in sys.path:
            sys.path.insert(0, folder)
        sys.argv = [script, *arguments]
        try:
            runpy.run_path(script, run_name='__main__')
        except SystemExit as stop:
            if stop.code not in (None, 0):
                raise


if __name__ == '__main__':
    main()
