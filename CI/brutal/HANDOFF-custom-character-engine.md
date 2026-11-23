# Handoff: general custom-character engine work (branch `claude/bru-custom-character`)

Status: **draft, incomplete, not ready for merge.** This branch adds general engine
systems only. **No character-specific integration, asset conversion, likeness or
parity with any source game was done.** Every test uses generated geometry or
stock Morrowind data. No game assets, binaries, logs or saves are in the repository.

- Base: `integrate/bru-kurenai` @ `92acfed`
- PR: https://github.com/sunhawken/openmw/pull/20 (draft)
- Commit list: `git log --oneline 92acfed..claude/bru-custom-character`

## 1. Subsystems

| Subsystem | Files | State |
|---|---|---|
| Multi-piece equipment (`AttachTo:<bone>[\|label]` nodes) | `components/sceneutil/bonepieces.*`, `apps/openmw/mwrender/{animation,npcanimation}.*` | Done for NPC and player parts; unit and in-engine tested. Creature weapons not done. |
| Move set core (data-driven combos) | `files/data-brutal/scripts/brutal_morrowind/moveset/core.lua` | Done; 25 LuaJIT tests |
| Move set engine layer, settings page, inputs | `moveset/{player,menu,input}.lua`, `l10n/BrutalMoveset/en.yaml` | Done; 18 in-engine checks. Save/reload not tested in-engine. |
| Lua `animation.getBoneTransform(object, bone)` | `apps/openmw/mwlua/animationbindings.cpp` | Added, used by the rig test. Not yet in the Lua API docs. |
| Collada exact node and clip names | `components/resource/colladanames.*`, `scenemanager.cpp` | Done; unit and in-engine tested |
| Collada arbitrary skeleton root (no `bip01`) | `components/resource/keyframemanager.*` | Done; whole-body only (no torso/arm split) |
| Collada clips with absolute keys | `components/sceneutil/osgacontroller.*` (`EmulatedAnimation::mAbsoluteKeys`) | Done; 7 unit tests |
| Collada parse failure (unterminated buffer) | `scenemanager.cpp`, `colladanames.cpp` (`readColladaForReader`) | Fixed; regression test; 0 failures in 40 stressed loads |
| Collada per-model summary log | `scenemanager.cpp` ("Collada model ...") | **Wrong**: it reports 0 skins / skeleton missing for working rigs. See bug B1. |
| GLB/glTF → Collada converter | — | **Not started** |
| Bone-relative oriented hitbox events | — | **Not started** |
| Terrain or no-object ray occlusion regression | — | **Not started** (the code fix is in `player.lua`) |
| Player-path rig test (player record using a `.dae`) | — | **Not started**; only the NPC path is tested |
| Save/reload and active-set replacement regressions | — | **Not started** |

## 2. Root causes found and fixed

1. **Collada bone and clip names.** OSG's reader names bones after the XML `id` and
   clips after the `animation_clip` id, not their `name`. The loader now reads the
   names from the document and renames bones, update callbacks, skin influence maps,
   channel targets and clips. Explicit names are kept exactly, underscores included.
   Nodes without a `name` keep the old underscore→space rule, but only when the result
   doesn't collide with another node's id or name.
2. **Skeleton root.** Keyframe controllers were only created under a bone named
   `bip01`. Now, when no `bip01` exists, the topmost bone gets a whole-body controller.
   Stock rigs keep the four-part split.
3. **Clips starting at a nonzero time.** The controller sampled clips at
   `time - clipStart`, but Collada keys use the absolute timeline, so those clips froze
   on their first key. Clips from the Collada loader are now flagged `mAbsoluteKeys`
   and sampled at the timeline time, so each channel keeps its own offset. Other
   producers keep relative sampling.
4. **Intermittent "Extra content at the end of the document".** OSG 3.6.5
   `daeReader::convert(std::istream&)` copies exactly the stream length into a
   `std::vector<char>` and calls `openFromMemory(uri, &buffer[0])`, which takes no
   length, so the parser runs past the end. The loader now passes a NUL-terminated
   copy. A mutex was tried first; it had no effect (OSG already serializes reads with
   `SERIALIZER()`) and was removed.
5. **Move set review fixes.** One timeline per update (remaining time, cooldowns,
   buffer); authored event order and ordinals; hit windows on the segmented timeline;
   per-instance window identity; ray hit flag for occlusion.

## 3. Build (cloud, Ubuntu 24.04)

The packages, the git clones of pinned dependencies (GitHub zip downloads are blocked
by the proxy, git clone works) and the configure flags are those in
`CI/brutal/build-cloud.sh` (on the `claude/kurenai-no-pcsx2` branch). Unit tests also need
googletest:

    git clone --depth 1 --branch release-1.12.1 https://github.com/google/googletest <deps>/googletest
    cmake ... -DBUILD_UNITTESTS=ON -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<deps>/googletest
    ninja openmw openmw-tests

`apps/components_tests` is not registered in this branch's CMake; the new C++ tests
live in `apps/openmw_tests`.

## 4. Tests and latest results

All in-engine tests: run under Xvfb, muted (`--no-sound`), with their own temporary
`--config` and `HOME`, and no screenshots. Results are read from the engine log.
Arguments: `<brutalmorrowind binary> <Morrowind "Data Files" dir with Morrowind.esm/.bsa>`.

| Command | Latest result |
|---|---|
| `./openmw-tests` | 526 / 526 pass |
| `luajit files/data-brutal/scripts/brutal_morrowind/moveset/tests/test_core.lua` | 25 / 25 pass |
| `CI/brutal/tests/bonepieces/run.sh BIN DATA` | 5 / 5 pass |
| `CI/brutal/tests/moveset/run.sh BIN DATA` | 18 / 18 pass |
| `CI/brutal/tests/rig/run.sh BIN DATA` | NPC path, 3 rigs: all bone, clip and motion checks pass after the fixes. **The script still exits FAIL** because of the broken summary-line check (B1). |
| `RIG_STRESS=5 CI/brutal/tests/rig/run.sh BIN DATA` (8 rigs at once), 5 runs | 0 load failures in 40 loads. Per-run checks 99/100, 99/100, 100/100, 100/100, 99/100; the 3 failures are B2. Every run exits FAIL because of B1. |

Rig test environment variables: `RIG_STRESS=N` (extra custom rigs), `RIG_TEST_TIMEOUT`
(seconds, default 60), `RIG_TEST_LOG` (copy of the engine log). The move set test takes
`MOVESET_TEST_LOG`.

## 5. Generated fixture contract

`python3 CI/brutal/tests/fixtures/make_collada_rig.py OUT_DIR [--name N] [--bip01 [--legacy]]`

It writes `N.dae`, `N.txt` (text keys) and `N_tex.png` (2x2). Z-up, `unit meter=1`;
the values are Morrowind units, unscaled.
- **Custom (default):** bones `rig_root > pelvis > spine_lower > spine.upper > {neck,
  arm_l > "arm l", arm_r > hand_r}`. Ids are an encoding that keeps `arm_l` and
  `arm l` distinct, so the names are recoverable only from `name`.
- **`--bip01`:** the same rig with stock names (`Bip01`, `Bip01 Pelvis`, ...),
  exporter style: underscored ids, exact `name`.
- **`--legacy`** (with `--bip01`): ids only, no `name` attributes.
- **Skins:** `body_skin` (pelvis, spine, neck) and `arms_skin` (arms, hand) on the one skeleton.
- **Clips** (absolute timeline): `sway` 0–2 s (rotates the spine top bone),
  `wave_hello` 2–3 s (the left arm bone), `Slash Combo.2` 3–3.8 s (the right hand bone,
  with an extra `hit` text key at 3.4).
- **Text keys:** lower-cased group, `start`/`stop`.

## 6. Open bugs

- **B1.** The `Collada model ...: N skinned meshes, M clips, skeleton found|missing`
  Verbose line reports `0 skinned meshes, skeleton missing` for rigs that work in game.
  It is probably logged where `colladaDescriptionVisitor` is not the populated one, or
  before the rebuild. Fix the placement, or count `RigGeometryHolder`s in the final
  node. Until then, `rig/run.sh` always exits FAIL on its summary check.
- **B2.** In stress mode the "no clip" control can move 5–18 units, because the copied
  NPCs (Fargoth template with wander AI) walk and turn, and the check uses world
  transforms. Use a template with no AI or wandering, or measure in the actor's local
  frame (inverse of the root transform).
- **B3.** One early run showed 0.00 motion for both Bip01 rigs. It was not reproduced
  in later runs; it was possibly the parse failure fixed in 2.4 (a rig loading as an
  error marker). Keep it in mind.
- **B4.** The occlusion check for rays that hit terrain or geometry with no object has
  no regression; the wall test also passed with the old check.
- **B5.** Stock NPC body parts log "Can't find attachment node" on non-Bip01
  skeletons. Expected, but noisy.

## 7. Next actions, in order

1. Fix B1 and B2. Rerun `rig/run.sh` and `RIG_STRESS=5` (5 runs, keep the logs).
2. Player track: a test that generates a throwaway plugin at run time overriding the
   `player` NPC record's model with a generated rig, then checks bones and clips through
   `animation.getBoneTransform` on the player. Never commit that plugin.
3. Real save/reload regression for move sets (in-game save, then load, through MENU
   `openmw.menu`), and replacing the active set mid-move (no animation or hit-window
   leak). Decide and document whether a move in progress survives a save (the core
   currently drops it).
4. Terrain or no-object ray occlusion regression (B4).
5. Bone-relative oriented hitbox events: an event field giving a bone, position,
   rotation and size; test with `getBoneTransform` and a box-overlap check on
   generated rigs.
6. GLB/glTF → Collada: first check `assimp export in.glb out.dae` (apt `assimp-utils`)
   on generated GLB fixtures for hierarchy, inverse binds, weights, materials and
   images, and every clip as its own `animation_clip` with a correct `name`. Then
   decide whether to write a converter. Document units: glTF is meters and Y-up, while
   this loader expects Z-up and Morrowind units.
7. Upper-body blending for non-Bip01 skeletons (a declared bone mapping), root motion
   for arbitrary roots (accumulation currently looks up `bip01`), and creature-weapon
   pieces.
8. Refresh the PR description, then run a code review before marking it ready.
