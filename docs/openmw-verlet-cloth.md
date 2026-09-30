# Hair and cloth chains (physics-verlet)

Hair, skirts and similar hanging parts are simulated with the method of
[sunhawken/physics-verlet](https://github.com/sunhawken/physics-verlet), made 3D. Nothing else is added
to the algorithm: no air drag, wind, springs or tuning sliders.

## Method (`apps/openmw/mwrender/verletclothcontroller.cpp`)

- Every particle keeps its position and its previous position; velocity is the difference.
- Gravity is the only force: 9.81 m/s^2 at 72.5 units per metre (about 711 units/s^2). Fixed 60 Hz step.
- Each step the constraints are applied 10 times, in this order (as in `rope.c` / `cloth.c`):
  1. `world_collide`: overlapping particles are pushed apart (radius 0.75 units).
  2. `constrain_distance_from_point(maxd = 0)`: the first `verlet_pin_count` particles of every chain
     sit on their animated position.
  3. `constrain_distance_between_objects`: consecutive particles of a chain may not be further apart
     than at rest.
  4. Neighbouring chains (each chain is linked to its two nearest) may not be further apart than
     1.1 x their rest distance at the same level, so the whole sheet moves together.
- The only additions the 2D library does not have: immovable body capsules (torso, thighs, calves) and
  the actor's floor, so the cloth cannot pass through the body.

## Authoring

A chain root carries `OPENMW_VERLET_CLOTH` and `verlet_count=N` (optionally `verlet_pin_count=M`,
default 3) as string extra data. The numbered bones `Name_01 ... Name_NN` form the chain. All chains under
the same parent bone are simulated together by one controller. Bones are moved by translation only.

## Setting

`[Game] verlet enabled = true` switches the simulation on or off. There is no Options tab.
