# Kurenai + Bru 0.2.1 with Jolt

This integration starts at Kurenai commit `d495ade90f071afe43d0a9c5a6c5ac8d82b3c6d4` and imports the Bru feature delta from `sunhawken/bru` (`61f35eaea3522781004743b39054cc00230c9ff2`), followed by upstream Brutal Morrowind 0.2.1 (`16843789f84d588334c74417ee25a8cdf3d71e80`).

The native Jolt backend remains in use. Bru NPC ragdolls use eleven Jolt bodies and swing/twist constraints, including living knockdowns, head/torso/limb separation, and the existing BSP1/LSP1 save formats. Creature rigs, buoyancy, anatomical joint controls, secondary kick contacts, released equipment hulls and player momentum use Jolt. Kurenai's existing ragdoll path remains available when the Bru ragdoll option is disabled. Its rendering, body customization and cloth code is retained.

The 0.2.1 Linux relocation/importer fixes are included. CI uses the Kurenai Jolt build configuration; the release workflow consumes the existing Windows artifact name. Obsolete, unreferenced Bullet ragdoll helper headers are omitted.

## Validation

On Ubuntu 24.04 under WSL, GCC 13, CMake/Ninja, Jolt 5.3.0:

- Release engine and Qt 6 launcher compile and link successfully.
- All 504 OpenMW tests pass, including ten integration/serialization tests added for this port.
- Actual Jolt tests cover eleven-body simulation, floor collision, cleanup, head detachment, torso and limb persistence, secondary kick contact deduplication and overlapping spawn protection.
- Serialization tests cover both wire formats, rotated actor frames, truncation, invalid masks, non-finite values, invalid rotations and oversized offsets.
- Engine `--version` starts and exits successfully. Its linked libraries contain no Bullet dependency.

Example configuration (install the usual OpenMW/Jolt build dependencies first):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_OPENCS=OFF -DBUILD_LAUNCHER=ON -DBUILD_WIZARD=OFF \
  -DBUILD_ESSIMPORTER=OFF -DBUILD_BSATOOL=OFF -DBUILD_ESMTOOL=OFF \
  -DBUILD_NIFTEST=OFF -DBUILD_NAVMESHTOOL=OFF -DBUILD_PHYSICSOBJECTTOOL=OFF \
  -DOPENMW_USE_SYSTEM_MYGUI=OFF -DBUILD_UNITTESTS=ON -DOPENMW_LTO_BUILD=OFF
cmake --build build --target openmw openmw-launcher openmw-tests
./build/openmw-tests
```

The CMake targets are named `openmw` and `openmw-launcher`; the resulting programs are `brutalmorrowind` and `brutalmorrowind-launcher`.

## Validation limits

No playthrough with owned Morrowind/Dark Messiah assets or Windows binary validation was performed for this integration. The Jolt implementation preserves Bru's gameplay interfaces and save formats; its solver response is not claimed to be identical to Bullet. The earlier upstream 0.2.1 release mirror in `sunhawken/openmw-rtx` is a separate upstream build, not this Jolt integration.
