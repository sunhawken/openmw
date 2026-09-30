# OpenMW `official-jolt-ragdoll` — fast cloud setup (notes)

Fork: https://github.com/sunhawken/openmw, branch `official-jolt-ragdoll` (OpenMW 0.49 + Jolt physics).

## Repeat it
1. In the session, attach repos with `add_repo` (sunhawken/openmw needs `push`; jrouwe/JoltPhysics is read-only, anonymous git works).
2. `bash setup.sh` (deps, pre-clones, cmake, `ninja openmw`; ~20-30 min first build, ccache after).
3. Fetch game data (below), then `bash run.sh`.

## Gotchas found
- FetchContent `github.com/.../archive/*.zip` returns **403** in the sandbox (repo not attached). Workaround: `git clone` each dep and pass `-DFETCHCONTENT_SOURCE_DIR_<NAME>=`. Needed: JoltPhysics v5.3.0, OpenMW/recastnavigation @03259f3, MyGUI 3.4.3.
- Ubuntu 24.04 MyGUI is 3.4.2 (< 3.4.3 required) → build MyGUI via FetchContent (`-DOPENMW_USE_SYSTEM_MYGUI=OFF -DMYGUI_STATIC=ON`).
- System OSG 3.6.5 is fine. Skip launcher/wizard/opencs to save build time.
- Long shell `sleep` is blocked; use background `until` loops / Monitor.

## Game data
User's Dropbox has `/Morrowind/Morrowind.esm` (79.8MB) and `Morrowind.bsa` (310MB). Use the Dropbox `download_link` tool (single-use, 15 min) then `curl -L` into `/home/user/morrowind-data`. Do not commit game data.

## Status
See SPEED.md for the fast-path notes; in-game results are appended below when done.

## Speed tips (future builds)
- `setup.sh` now builds with `-O1 -g0`, ccache (20G), mold, `-j$(nproc)`, and only the `openmw` target (no launcher/opencs/tools).
- Edit-test loop: `rebuild.sh run` rebuilds only what changed and launches.
- Cold container = empty ccache, so the first build is always ~20-30 min. To skip it: tar `openmw-build/` (or just `openmw`+`resources`) and keep it somewhere that survives sessions (Dropbox only takes text files via the tool; a GitHub release on a repo you own is an option), then untar into `/home/user/openmw-build`.
- Don't run the game and a build simultaneously on 4 cores/15GB.

## In-game result (2026-09-30)
Reached gameplay: Seyda Neen Census Office, HUD + NPC rendered (`ingame-seyda-neen.png`), Jolt physics threads active.
Issues hit and fixes:
- **Crash dialog "root widget '_Main' in openmw_settings_window.layout not found"**: branch bug. Line 1562 has `NIF's` inside an attribute; MyGUI's own XML parser treats `'` as a quote. Fixed locally by rewording to `NIF-authored` (already committed on this branch). Not yet pushed to the fork. Diagnose via `omw-config/MyGUI.log`.
- `--new-game` quit right after loading; use `--start "Seyda Neen, Census and Excise Office"` instead (run.sh does this).
- Audio device missing (harmless); `meshes/snow.nif`/`blizzard.nif` missing (Bloodmoon, harmless).
- `--start` drops the player at 0,0,0 so "Player position has been reset due to falling into the void" repeats. Expected for that spawn, not a ragdoll bug.
- Screenshots: `DISPLAY=:99 import -window root x.png` (imagemagick, xdotool, xvfb are in setup.sh).

## Skip the 25-min build (cache branch)
Branch `openmw-build-cache` of this repo holds `openmw-runtime.tar.xz` (stripped binary + resources, 11MB) and `ccache.tar.xz` (47MB) plus `restore.sh`:
`git clone --depth 1 -b openmw-build-cache https://github.com/sunhawken/claude-code-notes /home/user/omw-cache && bash /home/user/omw-cache/restore.sh`
Then apt deps (setup.sh's install line), data download, `run.sh`. To rebuild with ccache, clone openmw + deps and run setup.sh's cmake with the same flags. Refresh the cache after big changes (repeat the strip+tar+push from a separate clone).

Prebuilt binary + ccache (skips the compile): branch `openmw-build-cache` of https://github.com/sunhawken/claude-code-notes (see restore.sh there).
