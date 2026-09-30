# Runtime Verlet cloth chains

This branch supports lightweight runtime Verlet secondary motion for sequential skinned bone chains used by capes, skirts, long hair, tails, cords, and similar meshes.

This document reflects the current `official-jolt-ragdoll` implementation as of 2026-09-30. When debugging a converted outfit, use the branch source as the final authority because the solver and UI have evolved beyond the original prototype.

## Origin

The original OpenMW implementation was adapted from the current-position / previous-position Verlet integration and distance-constraint technique used by:

[Josephbakulikira/Cloth-Simulation-With-python---Verlet-Integration](https://github.com/Josephbakulikira/Cloth-Simulation-With-python---Verlet-Integration)

The first OpenMW commit that added the runtime chain solver was:

`b830234f08ae0a8041b972419912a22ae30f043f` — **Add runtime Verlet cloth chain solver**

The current OpenMW code is substantially more specialized than that source demo. It adds OpenMW bone-chain integration, NIF metadata, animated attachment handling, soft roots, body/leg/ground collision, stable timing, motion inertia, world-velocity air drag, airflow shape response, rotation carry, shape guards, sleeping, and full skinned-mesh regression checks.

## Why bones instead of direct cloth vertices?

OpenMW actor meshes are already skinned through a skeleton. The Verlet system therefore drives authored secondary bones rather than replacing the renderer with a separate CPU cloth mesh.

A chain such as:

```text
Cape_01 -> Cape_02 -> Cape_03 -> ... -> Cape_09
```

acts as a set of Verlet particles connected by fixed-distance constraints. Existing skin weights then deform the visible mesh.

This keeps the normal Morrowind/OpenMW skinning pipeline intact and lets Blender-authored weights define which parts of the surface follow each simulated bone.

## Runtime ownership

A bone used by a Verlet chain should not also be controlled by Jiggle. The runtime detects secondary-motion controllers and prevents duplicate controller ownership.

Verlet, Jiggle, and direct/author-authored secondary-motion systems are separate features. Do not assume a setting in one tab changes another system.

## Chain naming

The runtime resolves numbered chains from a marked root.

For example, a root named `v_01` with `verlet_count=8` resolves:

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

The same rule works for names such as `Cape_01`, `SiffHairA_01`, or `SiffSkirtL_01`.

Keep the numeric suffix at the end of the name. The bones should form the same nested parent/child sequence in the NIF.

## NIF metadata

Put an `OPENMW_VERLET_CLOTH` description / `NiStringExtraData` marker on the first bone of each numbered chain and include the desired `verlet_*` properties.

A compact example:

```text
OPENMW_VERLET_CLOTH;
verlet_cloth=true;
verlet_count=9;
verlet_pin_count=3;
verlet_soft_root_count=3;
verlet_soft_root_strength=0.25;
verlet_gravity=711;
verlet_wind=0;
verlet_friction=0.91;
verlet_iterations=32;
verlet_substeps=6;
verlet_max_step=6;
verlet_rotation_carry=0.95;
verlet_lateral_memory=0.24;
verlet_inertia=0.22;
verlet_inertia_max_acceleration=1800;
verlet_max_lateral_deviation=2.5;
verlet_air_drag=5;
verlet_air_drag_max_acceleration=1500;
verlet_air_shape_response=0.80;
verlet_velocity_deadzone=1.20;
verlet_contact_slop=0.08;
verlet_sleep_speed=2;
verlet_sleep_delay=0.8;
verlet_sleep_amplitude=0.18;
verlet_stable_timing=true;
verlet_project_velocity=true;
verlet_rest_collision_fit=true;
verlet_align_bones=true;
verlet_collide_legs=true;
verlet_ground=true
```

The parser accepts metadata tokens separated by semicolons, commas, or newlines, and accepts `=` or `:` between a key and value.

### Supported metadata

| Key | Meaning |
| --- | --- |
| `verlet_cloth` / `verlet_active` | Enables the marked chain. |
| `verlet_count` | Number of sequential numbered bones in the chain. |
| `verlet_friction` | Velocity retention. Higher values preserve more flowing motion. |
| `verlet_gravity` | Downward world-space acceleration. |
| `verlet_wind` / `verlet_wind_strength` | Procedural ambient wind strength. |
| `verlet_wind_frequency` | Ambient gust oscillation frequency. |
| `verlet_iterations` | Distance-constraint relaxation passes. |
| `verlet_substeps` | Integration substeps. |
| `verlet_max_step` | Maximum particle movement per integration step. |
| `verlet_pin_count` | Number of attachment-end bones held to the animated pose. A value above zero overrides the global Pin Count. |
| `verlet_soft_root_count` | Number of free bones immediately below the pinned region that receive an attachment-strength transition. |
| `verlet_soft_root_strength` | Strength of that soft-root transition, clamped to 0..1. |
| `verlet_velocity_deadzone` | Suppresses tiny residual velocities. |
| `verlet_contact_slop` | Small positional allowance for collision/contact stabilization. |
| `verlet_rotation_carry` | Fraction of parent-frame rotation carried into the simulated chain, 0..1. |
| `verlet_lateral_memory` | Preserves width/depth perpendicular to gravity, 0..1. |
| `verlet_max_lateral_deviation` | Emergency lateral shape bound; 0 disables it. |
| `verlet_stable_timing` | Enables timing-independent stability behavior used by the current Siff profile. |
| `verlet_project_velocity` | Projects corrected velocity so constraints/collisions do not inject artificial energy. |
| `verlet_rest_collision_fit` | Allows authored rest attachments to coexist with coarse body capsules. |
| `verlet_align_bones` | Aligns skinned chain sections coherently with the simulated/airflow frame while preserving bind twist/scale. |
| `verlet_inertia` | Bounded response to parent translation acceleration, 0..1. |
| `verlet_inertia_max_acceleration` | Clamp for parent-acceleration response. |
| `verlet_air_drag` | Sustained world-velocity trailing force. |
| `verlet_air_drag_max_acceleration` | Clamp for air-drag acceleration. |
| `verlet_air_shape_response` | Bends the volume/shape reference with airflow so shape guards do not cancel sustained trailing motion. |
| `verlet_sleep_speed` | Motion-speed threshold for sleeping; 0 disables sleep. |
| `verlet_sleep_delay` | Time below the sleep threshold before settling to sleep. |
| `verlet_sleep_amplitude` | Positional-amplitude threshold used by the sleep test. |
| `verlet_collide_legs` | Adds thigh/calf/foot capsule contacts for long hair or skirts. |
| `verlet_ground` | Enables actor-floor-plane ground contact. |

## Options -> Verlet

The normal user workflow is to leave authored NIF metadata in control.

Choose:

```text
Verlet Preset: NIF Metadata (Recommended)
Enable Verlet Cloth: ON
Use Global Overrides: OFF
Wind While Idle: OFF
Body Collision: ON
```

Selecting **NIF Metadata (Recommended)** explicitly enables Verlet, disables Global Overrides, disables idle wind, and enables body collision.

### What Global Overrides actually replaces

When **Use Global Overrides** is ON, the following authored values are replaced live for all active chains:

- gravity;
- friction;
- wind strength;
- wind frequency;
- iterations;
- substeps;
- max step.

It does **not** replace the advanced Siff motion metadata such as inertia, air drag, rotation carry, lateral memory, soft roots, stable timing, air-shape response, sleep, leg collision, or ground contact.

### Global controls that apply even with NIF Metadata selected

These are engine-level controls and remain global:

- **Enable Verlet Cloth** — master switch. OFF resets active chains to their authored rest pose.
- **Wind While Idle** — controls whether procedural wind continues when the actor is idle.
- **Idle Retention** — damping/retention used while idle.
- **Body Collision** — master body-capsule contact switch.
- **Body Collision Radius**.
- **Collision Padding**.
- **Pinned Top Bones** only when the NIF does not provide a positive `verlet_pin_count`.

This distinction matters when validating an outfit. A model can have correct NIF metadata but still behave differently if global collision or idle settings are changed.

Current default engine values are:

```ini
[Game]
verlet enabled = true
verlet use global settings = false
verlet friction = 0.94
verlet gravity = 4.0
verlet wind strength = 3.0
verlet wind frequency = 0.22
verlet iterations = 18
verlet substeps = 6
verlet max step = 3.0
verlet idle wind = false
verlet idle damping = 0.82
verlet pin count = 3
verlet body collision = true
verlet body collision radius = 12.0
verlet body collision margin = 1.5
```

For authored Siff meshes, keep Global Overrides OFF so the NIF values below are used.

## Current validated Siff profiles

These values are specific to the current Siff hair/regalia authoring and are not universal defaults.

| Metadata key | Siff hair | Siff regalia/skirt |
| --- | ---: | ---: |
| `verlet_count` | 11 | 9 |
| `verlet_pin_count` | 3 | 3 |
| `verlet_soft_root_count` | 4 | 3 |
| `verlet_soft_root_strength` | 0.35 | 0.25 |
| `verlet_gravity` | 711 | 711 |
| `verlet_wind` | 0 | 0 |
| `verlet_friction` | 0.94 | 0.91 |
| `verlet_iterations` | 32 | 32 |
| `verlet_substeps` | 6 | 6 |
| `verlet_max_step` | 6 | 6 |
| `verlet_rotation_carry` | 1.00 | 0.95 |
| `verlet_lateral_memory` | 0.14 | 0.24 |
| `verlet_inertia` | 0.30 | 0.22 |
| `verlet_inertia_max_acceleration` | 1800 | 1800 |
| `verlet_max_lateral_deviation` | 3.5 | 2.5 |
| `verlet_air_drag` | 5 | 5 |
| `verlet_air_drag_max_acceleration` | 1500 | 1500 |
| `verlet_air_shape_response` | 0.90 | 0.80 |
| `verlet_velocity_deadzone` | 1.20 | 1.20 |
| `verlet_contact_slop` | 0.08 | 0.08 |
| `verlet_sleep_speed` | 2 | 2 |
| `verlet_sleep_delay` | 0.8 | 0.8 |
| `verlet_sleep_amplitude` | 0.18 | 0.18 |
| `verlet_stable_timing` | true | true |
| `verlet_project_velocity` | true | true |
| `verlet_rest_collision_fit` | true | true |
| `verlet_align_bones` | true | true |
| `verlet_collide_legs` | true | true |
| `verlet_ground` | true | true |

The corrected Siff metadata specifically restored:

- hair `verlet_lateral_memory=0.14`;
- hair `verlet_air_shape_response=0.90`;
- regalia `verlet_air_shape_response=0.80`.

The existing skin weights were not blindly repainted. A binary NiSkinData audit found normalized skinning to floating-point tolerance with at most four influences per vertex and extensive weighting to the intended Siff secondary chains.

## Physical scale and gravity

Do not copy `711` blindly to every model.

For Siff, the reference calculation was approximately:

- actor height: 131 game units;
- assumed physical height: 1.81 m;
- scale: about 72.5 game units/m;
- Earth gravity: `9.81 * 72.5 ~= 711 units/s^2`.

If another converted rig uses materially different world scale, recalculate gravity to keep acceleration physically consistent relative to that actor.

## What the motion terms do

Use the controls for different jobs rather than trying to solve every problem with weight paint:

- **Friction** controls retained velocity/flow.
- **Gravity** establishes downward acceleration and drape.
- **Inertia** reacts to starts, stops, jumps, and parent acceleration.
- **Air drag** creates sustained trailing motion while the actor continues moving at steady speed.
- **Air shape response** lets the width/volume reference bend with airflow instead of forcing the cloth back toward an upright bind contour.
- **Rotation carry** carries turning/orientation changes coherently into the chain.
- **Lateral memory** helps preserve width/depth across the hanging surface.
- **Soft roots** keep the first free section close to the body while transitioning smoothly into fully dynamic motion.
- **Pinned roots** define the rigid attachment.
- **Alignment** prevents translated bone centers from shearing the skinned surface during strong lift/trailing poses.

If a correctly weighted free region lags on acceleration but stays glued to the actor at steady speed, inspect air drag and airflow shape response before repainting the mesh.

## Body, leg, and ground collision

With global **Body Collision** enabled, the controller builds torso capsules from the owning actor skeleton around:

```text
Pelvis -> Spine -> Spine1 -> Spine2 -> Neck
```

When `verlet_collide_legs=true`, long chains can additionally collide with thigh/calf/foot regions. This is especially important for skirts.

When `verlet_ground=true`, particles are constrained above the actor floor plane and downward velocity is removed at contact.

These are lightweight character-relative contacts. They are not a replacement for full environment cloth collision against arbitrary walls and terrain geometry.

## Weight painting

Physics metadata does not replace correct skinning.

For each converted NIF:

- keep finite, nonnegative weights;
- merge duplicate influences;
- keep no more than four nonzero influences per vertex;
- normalize each weighted vertex;
- keep rigid body/armor regions on their authored skeleton weights;
- blend only the intended attachment transition into secondary chains;
- use neighboring chain sectors and neighboring chain levels for the free surface;
- avoid repainting an already-correct mesh merely to change motion strength.

A useful four-way interpolation for a vertex between chain sectors A/B and levels i/i+1 is:

```text
A_i     = (1-a) * (1-h)
A_(i+1) = (1-a) * h
B_i     = a * (1-h)
B_(i+1) = a * h
```

where `a` is the fraction between neighboring chains and `h` is the fraction along the local chain segment.

At the attachment band, blend those chain weights with the original skeleton weights instead of abruptly replacing them.

## Validation

Do not treat a NIF that merely parses as validated.

### Structural checks

Verify:

- every numbered chain exists;
- every root has the Verlet marker;
- `verlet_count` matches the chain length;
- chain parenting is correct;
- the skeleton parent exists;
- all weighted bones exist;
- all skin weights are finite/nonnegative;
- weight sums are normalized;
- there are at most four nonzero influences per vertex;
- bind/skin transforms remain finite and invertible;
- the rigid region did not accidentally gain chain weights;
- the free region is not still held to Head/Pelvis by stray weights.

### Headless Siff regression suite

The reproducible test workflow is in:

`scripts/siff_verlet_tests/README.md`

The final verification entry point is:

`scripts/siff_verlet_tests/verify_final.py`

The harness executes the real controller C++ through a minimal scene/matrix adapter and uses full Siff skeleton/skin fixtures. It checks:

- walk/run/jump in all eight compass directions;
- 26 pull/impulse directions;
- 20/30/60/120/144 fps stress;
- variable timing, stalls, rewind, teleport, settle, sleep, and wake;
- drag-on/off trailing comparisons;
- walking versus running response;
- continuous full-speed runs at 320 units/s;
- first-free-segment root lift;
- complete skinned-mesh surface width/area rather than only bone-center paths.

This is strong numerical validation, but it is still not a substitute for a rendered OpenMW gameplay test.

### In-game checks

Test at minimum:

1. idle settle;
2. forward/back/left/right/diagonal walking;
3. the same directions at running speed;
4. jump and landing;
5. hard stops;
6. rapid 90-180 degree turns;
7. sustained forward and backward running;
8. re-equip/reload;
9. body/leg contact;
10. ground contact.

Watch for root detachment, explosive motion, flattening, permanent oscillation, skirt penetration, incorrect pull direction, and a free section that remains glued to the body at steady speed.

## Packaging

Do not make a fork-specific Verlet NIF the only version of an outfit unless the package explicitly requires this OpenMW build.

Preferred distribution:

1. normal Morrowind/OpenMW-compatible base outfit;
2. separate **Verlet Physics** overlay containing only the NIFs that differ.

Example:

```text
Meshes/
  Siff/
    siff_hair.nif
    siff_regalia.nif
README.txt
```

Document the required OpenMW branch/build, load order, physics meshes, chain counts, important metadata, and whether validation was structural, headless-controller, or actually rendered in game.

## Jiggle Quick Setup for converted outfits

Jiggle is separate from Verlet, but converted body/clothing meshes may use both systems for different regions.

The Advanced Jiggle Setup window now has a **Quick Setup** tab. For the normal workflow:

1. equip the body/clothing/armor to tune;
2. adjust the normal Jiggle sliders;
3. open **Advanced Jiggle Setup -> Quick Setup**;
4. click **Add Current Outfit to Jiggle**.

That one action:

- detects the currently worn mesh;
- enables Jiggle/auto-rig;
- enables player Jiggle;
- removes blacklist entries blocking that mesh;
- saves the current live breast/butt offsets for that mesh.

Other Quick Setup actions are:

- **Save Current Outfit Tuning for Player Only**;
- **Exclude Current Outfit**;
- **Remove Saved Tuning**.

The manual Mesh Z Offsets, Mesh Blacklist, Player/NPC Rules, and Glute Physics tabs remain available for edge cases.

## Troubleshooting

### Verlet does not move

Check, in order:

1. **Enable Verlet Cloth** is ON.
2. The model is running on the `official-jolt-ragdoll` build, not stock OpenMW.
3. The root has the Verlet marker and a positive `verlet_count`.
4. Every numbered chain bone exists.
5. The free surface is actually weighted to the secondary bones.
6. **NIF Metadata (Recommended)** is selected / Global Overrides is OFF for authored Siff tuning.
7. The user `settings.cfg` is not forcing unexpected global values.
8. The OpenMW log reports that the Verlet roots attached.

### Options window reports `_Main` not found

A branch regression previously placed `NIF's` inside a double-quoted MyGUI XML attribute. MyGUI's parser treats the apostrophe as a quote and failed the entire layout, producing the misleading "_Main not found" fatal error.

The current branch uses `NIF-authored` instead. If an old local build still shows the crash, update the branch, rerun CMake configuration so resources are recopied, rebuild, and make sure the runtime copy of:

`resources/vfs/mygui/openmw_settings_window.layout`

matches:

`files/data/mygui/openmw_settings_window.layout`.

## Source paths

Current implementation:

```text
apps/openmw/mwrender/verletclothcontroller.hpp
apps/openmw/mwrender/verletclothcontroller.cpp
apps/openmw/mwrender/animation.cpp
components/settings/categories/game.hpp
files/settings-default.cfg
files/data/mygui/openmw_settings_window.layout
scripts/siff_verlet_tests/
```

For conversion/weight-paint guidance, also read:

[https://github.com/sunhawken/claude-code-notes/blob/master/docs/morrowind-outfit-conversion-guide-and-validation-notes.md](https://github.com/sunhawken/claude-code-notes/blob/master/docs/morrowind-outfit-conversion-guide-and-validation-notes.md)
