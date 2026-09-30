# Making everything fast in the cloud session

Goal: shortest wall-clock from fresh session to in-game. Cold-start order matters more than any single flag.

## Critical path (do these in parallel, not in sequence)
1. Start `apt-get install` (deps) **in the background** while cloning repos.
2. Clones are independent: openmw, JoltPhysics, recastnavigation, MyGUI. The git proxy caps **2 concurrent smart-HTTP ops per repo**, so run different repos in parallel, never the same repo twice (429s otherwise).
3. Fetch game data (Dropbox links) while the compile runs; it only matters at launch time.
4. Start `ninja openmw` with `run_in_background`; watch with an `until grep` loop or Monitor. Never `sleep` in foreground (blocked).
5. Issue independent tool calls in one turn (e.g. both Dropbox `download_link` entries in a single call, `entries` takes up to 25).

## Per-tool tips
- **git**: `--depth 1 -b <branch/tag>`; recastnavigation needs a specific commit so it needs full history (small repo, fine). Use `GIT_LFS_SKIP_SMUDGE=1` for Jolt. Pre-clone deps and pass `-DFETCHCONTENT_SOURCE_DIR_*` (archive zips are 403).
- **apt**: `--no-install-recommends`, one combined install line, `-qq`. Only the needed `-dev` packages (no Qt if launcher/OpenCS off... but `qttools5-dev` is still needed by CMake config unless `BUILD_LAUNCHER`/`WIZARD`/`OPENCS` and Qt are all off; test before pruning).
- **cmake/ninja**: `-G Ninja`, `-j$(nproc)` (4 cores here), only target `openmw`. Skip launcher/wizard/opencs/tools/tests.
- **compiler**: `-O1 -g0`; ccache (`--max-size=20G`); `-fuse-ld=mold`. Optional: `OPENMW_UNITY_BUILD=ON` cuts compile time but needs more RAM (15GB box; watch for OOM). PCH is already used by the project.
- **linking MyGUI static**: avoids a separate shared lib build/install step.
- **downloads (Dropbox)**: links are single-use and expire in <=15 min; request them right before `curl -L`. Don't HEAD/preview them. Download BSA (310MB) in background; ESM alone is not enough to start.
- **display/test**: Xvfb + `LIBGL_ALWAYS_SOFTWARE=1` (llvmpipe). Use `--skip-menu --new-game` to go straight in; lower resolution (e.g. 800x600) and disable shadows/distant land/water reflections in `settings.cfg` for a faster software render.
- **screenshots**: `import -window root out.png` (ImageMagick) or OpenMW's own screenshot key; then `Read` the PNG.
- **rebuilds**: use `rebuild.sh run`; only changed TUs recompile.

## Bigger wins if you will repeat this a lot
- Persist a tarball of `openmw-build/` (binary + ccache dir `~/.cache/ccache`) outside the container (GitHub release on your own repo) and restore it at session start: cold build 20-30 min -> a few minutes of incremental work.
- Keep a `SessionStart` hook (see the `session-start-hook` skill) that runs deps install and clones automatically so it is done before you ask.
- Trim `data=` to just Morrowind.esm/bsa to shorten startup (no other content loaded).

## Time budget reference (this 4-core / 15GB box)
apt deps ~2-3 min, clones ~1-2 min, configure ~1 min, cold `ninja openmw` ~25+ min (1106 steps), data download ~1-2 min.
