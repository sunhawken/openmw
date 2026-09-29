# OpenMW Verlet v1.2

This branch build bundles the current runtime secondary-motion work:

- Verlet cloth chains with gravity, movement pull, idle settling and wind controls.
- Configurable pinned top bones for cape attachment regions.
- Torso capsule body collision using Pelvis/Spine/Neck bones.
- Dedicated in-game Verlet settings tab.
- Separate Wiggle enable/disable toggle; Jiggle remains independent.
- Expanded Jiggle quality-of-life controls:
  - player and NPC default toggles,
  - per-NPC Force ON / Force OFF rules by display name,
  - per-mesh and scoped Player/NPC breast/butt Z offsets,
  - in-game mesh blacklist editing,
  - collapsible Morrowind-style Mesh / NPC setup panel.
- Rose Sorceress cape arm weighting fix remains a content-side NIF change rather than an engine-side animation override.

Build label: OpenMW-Verlet-v1.2
