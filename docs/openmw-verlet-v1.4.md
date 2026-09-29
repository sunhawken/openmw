# OpenMW Verlet v1.4 - Glute (Booty) Physics

This build adds Naturalis-style glute physics to the Jiggle system, ported from the BootyMagic
module of everlaster's Naturalis v74 VaM plugin (`GluteJointPhysicsHandler`,
`GluteForcePhysicsHandler`, `GluteGravityPhysicsHandler`).

## What changed

- Butt jiggle bones ("Bip01 L/R Butt", auto-rigged or pre-rigged) now get their own joint response
  instead of sharing the breast softness / quickness / mass-response tuning:
  - separate swing (rotation spring/damper) and in/out depth (in/out spring/damper) springs,
    evaluated with Naturalis's own softness, mass and quickness curves;
  - glute mass (1-3 kg, 78-92% on the joint), auto-derived from bone size or set manually;
  - depth in / depth out force response (motion pushing the glute into or out of the body);
  - glute gravity multiplier (Naturalis uses 0.5 for all glute directions);
  - Up/Down and Left/Right angle offsets (lift, and push together / pull apart);
  - self-collision now works along the glute depth axis for auto-rigged bones.
- The Advanced Jiggle Setup window has a new **Glute Physics** tab with all of the above as live
  sliders/toggles.
- The physics is expressed relative to Naturalis's default glute, so at default slider values the
  glutes keep the feel of the main Jiggle tab tuning, except for the 0.5 gravity multiplier.

## Settings ([Game])

| Setting | Default | Naturalis equivalent |
| --- | --- | --- |
| jiggle glute physics | true | BootyMagic enabled |
| jiggle glute softness | 70 | Glute Softness (Joint Physics) |
| jiggle glute quickness | 0 | Glute Quickness Offset |
| jiggle glute auto mass | true | Volume-based Glute Joint Mass |
| jiggle glute mass | 1.5 | Glute Weight (kg) |
| jiggle glute gravity | 0.5 | Glute gravity multipliers |
| jiggle glute depth in | 1 | Force Physics Depth In Multiplier |
| jiggle glute depth out | 1 | Force Physics Depth Out Multiplier |
| jiggle glute up down angle | 0 | Up/Down Angle Offset |
| jiggle glute left right angle | 0 | Left/Right Angle Offset |

## Not ported

Naturalis soft physics (per-vertex soft-body joints), morph-based force/angle morphing, friction,
hard colliders and clothing profiles depend on VaM's soft-body skin and morph system and have no
counterpart in OpenMW's bone-based jiggle.

Build label: OpenMW-Verlet-v1.4
