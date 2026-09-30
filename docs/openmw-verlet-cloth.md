# Runtime Verlet cloth chains

This branch supports a lightweight runtime Verlet solver for sequential skinned bone chains.

The implementation is adapted from the current/previous-position and distance-constraint method used by
[Josephbakulikira/Cloth-Simulation-With-python---Verlet-Integration](https://github.com/Josephbakulikira/Cloth-Simulation-With-python---Verlet-Integration).

## Why bones instead of direct mesh vertices?

OpenMW's actor meshes are already skinned through the skeleton. Driving an authored chain such as
`v_01 -> v_02 -> ... -> v_08` lets existing NIF skinning deform the cape without rebuilding the
renderer/skinning pipeline for a CPU-side per-vertex cloth mesh.

The first bone is pinned to its normal animated attachment. The remaining bone origins are Verlet
particles connected by fixed-distance constraints.

## NIF metadata

Put the following `NiStringExtraData` records on the first bone in a numbered chain:

```text
OPENMW_VERLET_CLOTH
verlet_cloth=true
verlet_count=8
verlet_friction=0.95
verlet_gravity=1.5
verlet_wind=8.0
verlet_wind_frequency=0.8
verlet_iterations=8
verlet_substeps=2
verlet_max_step=12.0
```

If the marked root is named `v_01`, `verlet_count=8` resolves:

```text
v_01
v_02
v_03
v_04
v_05
v_06
v_07
v_08
```

The same numbering rule works for names such as `Cape_01`.

### Settings

- `verlet_friction`: velocity retention per 30 Hz reference step. The source demo uses 0.95.
- `verlet_gravity`: world-space downward acceleration.
- `verlet_wind`: procedural horizontal air-force strength. Set to 0 for a source-style gravity-only simulation.
- `verlet_wind_frequency`: gust oscillation frequency.
- `verlet_iterations`: distance-constraint relaxation passes per substep.
- `verlet_substeps`: integration substeps per rendered frame.
- `verlet_max_step`: safety clamp for one particle integration step.

The chain should consist of undriven secondary bones with mesh weights authored in Blender. Do not put
independent `wiggle_*` controllers on the same bones; the Verlet chain takes ownership of all members.

## Live movement strength

The **Verlet** tab's **Motion Strength** slider ranges from **0x to 8x** and
works without enabling **Global Overrides**. It uses the existing `[Game]`
setting `verlet movement influence`:

- **0x**: disable movement-driven inertia, air resistance and root lift.
- **1x** (default): retain the NIF's tuned pull-back and upward trailing angle.
- **2x-8x**: amplify those forces and permit a steeper upward trailing angle.

The slider multiplies authored `verlet_inertia` and `verlet_air_drag`, including
the airflow acceleration limit. Root lift uses the authored
`verlet_air_shape_response`; its angle ceiling rises from 65 degrees at 1x to
at most 85 degrees. Profiles without these metadata controls keep their existing
gravity/wind behavior. Pins, soft roots, collision handling, shape preservation
and integration safety limits remain active throughout the range. The setting
is saved normally and takes effect on existing cloth when simulation resumes.
