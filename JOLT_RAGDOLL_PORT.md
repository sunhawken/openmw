# Jolt / Ragdoll edition

This branch is openmw-web with the engine features of
[`sunhawken/openmw` `official-jolt-ragdoll`](https://github.com/sunhawken/openmw/tree/official-jolt-ragdoll)
merged in. Everything outside `openmw/` (the launcher, dashboard, server, multiplayer, build
tooling) is openmw-web as it was, plus the build-script changes listed below.

## What came over

From the `official-jolt-ragdoll` branch (tip `9bff825aff`):

- **Jolt Physics instead of Bullet.** `apps/openmw/mwphysics` runs on Jolt 5.3.0
  (double precision, `components/nifjolt`, `components/physicshelpers`,
  `components/resource/physicsshape*`). Bullet is no longer a dependency.
- **Ragdolls** on death (`ragdollbuilder`, `skeletonmapper`), with a fallback to the normal death
  animation when a ragdoll cannot be built.
- **Dynamic objects and buoyancy**: loose items and ingredients are simulated bodies
  (`[Physics] enable dynamic objects / enable buoyancy / enable ragdoll`, new *Physics* settings
  tab, `collision-shapes.yaml`).
- **Secondary motion**: Jiggle bones (auto-rig, per-actor/per-NPC rules, per-mesh Z offsets,
  seam welding, NIF bone baker), direct Blender "Wiggle Bones" metadata, and Verlet cloth chains
  for capes/skirts/hair with body-capsule collision. Each has its own settings tab; the advanced
  Jiggle setup is a separate window (`openmw_jiggle_setup.layout`).
- **Body/appearance**: optional Curvy Body / Curvy Naked Body meshes, Hazaeki transformation
  body overrides, the head/hair swapper and the persistent live appearance window.
- **Rendering**: masked occlusion culling (`extern/maskedoc`, `components/occlusionculling`,
  `[Camera] occlusion culling*`) and soft shadows.
- **Gameplay/QoL**: mouse-flick directional melee attacks, container *Transfer All*, Goodbye
  always ends dialogue, runtime plugin cleaning of "Evil GMSTs" (`[Game] clean plugins`),
  self-healing settings/config, and Lua handlers that recover instead of needing a reload.

## How the merge was done

openmw-web's `openmw/` is a squashed snapshot of upstream OpenMW `bc1d9c97a3` plus the web
changes, so it shares no history with the fork. The snapshot was re-parented onto `bc1d9c97a3`
and the fork merged into it with a real three-way merge (the fork's upstream base is older, from
December 2025). Conflicts were resolved as follows:

- **Upstream drift** (Lua content bindings, ESM paths, clustered lighting replacing the old
  lighting-method setting, launcher/CS/CI files): the newer upstream side from openmw-web won.
- **Web adaptations** (`#ifdef __EMSCRIPTEN__` blocks, multiplayer `MWMP` hooks, the deferred
  settings save, synchronous data loading, NIF load stats, headless window manager) were kept
  exactly and the fork's changes were layered around them.
- **Fork features** were kept, ported to the newer upstream APIs where they collided
  (`ESM::Path::getNormalized()`, `setSimAnchorGrids` on the Jolt navigator signatures,
  upstream's RootCollisionNode handling in the Jolt NIF loader).

Three pre-existing bugs in the fork were fixed on the way: an extra `</Widget>` in
`openmw_settings_window.layout` that pushed the Wiggle and Verlet tabs outside the tab control,
`openmw_jiggle_setup.layout` missing from the resource copy list (so the advanced Jiggle window
had no layout to load), and `extern/maskedoc` not compiling with clang < 19 (`__cpuidex`).

## Web-specific changes for the new code

- **Jolt threads** (`mwphysics/physicssystem.cpp`): at most one Jolt worker under Emscripten.
  Workers come out of the fixed `PTHREAD_POOL_SIZE`, and the main thread waits on physics jobs,
  so a worker created past the pool could never start. One worker is what the Bullet build used.
- **Jolt SIMD** (`extern/CMakeLists.txt`): `USE_WASM_SIMD` for Emscripten. The web build's global
  `-msimd128` makes Jolt pick its SSE4.x paths, which need Emscripten's SSE4.2 emulation.
- **Jolt lifetime** (`apps/openmw/main.cpp`): the Jolt factory is not torn down when `main()`
  returns on the web, because the leaked engine keeps running from browser callbacks.
- **Stack size**: `-sSTACK_SIZE` and `-sDEFAULT_PTHREAD_STACK_SIZE` are 1 MB (Jolt's documented
  minimum; the physics job runs on a pthread).
- **Occlusion culling on wasm** (`extern/maskedoc`): the SSE4.1 rasterizer is compiled against
  Emscripten's SIMD128 emulation (`-msimd128 -msse4.1`), dispatch is pinned to SSE4.1 (there is
  no CPUID), and rounding-mode control is stubbed, because wasm always rounds to nearest, which
  is the only mode the SSE4.1 path asks for.

## Building

The flow is unchanged (`wasm-build/fetch-deps.sh`, `build-deps.sh`, `configure-openmw.sh`,
`link-openmw.sh`), except:

- `fetch-deps.sh` fetches Jolt v5.3.0 into `deps/src/JoltPhysics` instead of Bullet;
  `build-deps.sh` no longer builds Bullet.
- `configure-openmw.sh` points CMake's FetchContent at `deps/src/JoltPhysics` when it is there
  (otherwise configure downloads it) and drops all Bullet flags.
- `link-openmw.sh` links `_deps/joltphysics-build/libJolt.a` and `extern/maskedoc/libmaskedoc.a`
  by name. The link runs with `ERROR_ON_UNDEFINED_SYMBOLS=0`, so a missing archive would only
  show up at runtime.
- `server/Dockerfile.simpeer` no longer builds Bullet; the native peer gets Jolt from the engine's
  CMake too.
- `fsroot/` (the committed preload mirror of `resources/`) is synced with the merged engine:
  `defaults.bin` (the new settings), the new Physics/Jiggle/Wiggle/Verlet settings layout, the
  advanced Jiggle and head/hair windows, `collision-shapes.yaml`, the Transfer All container
  window and the merged l10n and chargen layouts.

## Verification status

- The merged engine builds and links natively (Linux, clang 18) with the web build's options
  (tools off, `openmw` target).
- Jolt 5.3.0 and `maskedoc` build with Emscripten 6.0 for wasm64 with `-msimd128`, using the
  flags from `configure-openmw.sh`.
- Every engine source with `__EMSCRIPTEN__` code, plus every source the fork touched (about 300
  files), passes an `em++` wasm64 compile check with the web build's flags. Two files fail only
  for lack of the web's Lua 5.4 and GLES headers in the check environment; both are unchanged
  from openmw-web.
- **Not yet done:** a full wasm engine bake (Jenkins `openmw-web-dev`) and an in-browser playtest,
  or the multiplayer harness against a Jolt-based peer. Physics runs on a different engine now,
  so peer and client simulation should be re-checked before this goes near `ovhcloud`.
