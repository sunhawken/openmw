# Verlet in-game preview workflow (Siff hair / regalia)

Headless, repeatable "walk / run / stop" previews of Verlet cloth in the real game.

1. `Siff - armor for work.rar` (Dropbox) -> `7z x` -> copy `Meshes/`, `Textures/` and the
   `.esp` (renamed `Siff.esp`) into `/home/user/siff-data`. Items: `siff_fullbody_armor`,
   `siff_regalia`, `siff_jewelry` (the `siff_fb_*` ids are body parts, not items).
   Hair/regalia are for FEMALE characters - `prep.sh` switches the player via `enableracemenu`.
2. Copy `vt-data/` to `/home/user/vt-data` (Lua test mod: global script walks the player on a
   timeline idle 3s / walk 4s / stop 3s / run 4s / stop 4s with walk/run animations, player
   script drives a side-view static camera; `tcl` is on because collision is broken headless).
3. `./shoot.sh OUTDIR` restarts the game, equips gear, sets clear weather + noon and saves
   ~34 cropped frames (`cap.sh`). Build GIFs with ImageMagick `convert -delay 22 -loop 0`.
   Change camera distance in `player.lua`, then `reloadlua` in the console (no restart).
4. Optional: add a temporary `VTRACE` Log line in `VerletClothController` and run
   `analyze.py omw-out-run2.log` for tip tilt / jitter numbers per gait.

Gotchas: Lua `self.controls` only works with `input.setControlSwitch(Controls,false)` but the
Jolt build still won't move a `tcl` player, so movement is done with `object:teleport` from a
global script; `camera.setStaticPosition` needs Static mode (set it, wait a frame); the console
key `` ` `` pauses the game - close it before capturing.
Software GL runs ~7 fps in-game and ~0.6 s per `import` screenshot.
