# Credits

## OpenMW

Brutal Morrowind is a fork of [OpenMW](https://gitlab.com/OpenMW/openmw) 0.51.0, GPLv3.
Its authors are listed in `AUTHORS.md`.

## Morrowind Modern Physics

The physical ragdoll and dynamic object bodies are derived from
[Morrowind Modern Physics](https://github.com/sunspotdark/morrowind-modern-physics) by sunspotdark,
commit `eb50a40`, GPLv3. The derived files and functions are marked in the source:

* `apps/openmw/mwphysics/ragdoll.cpp`, `apps/openmw/mwphysics/ragdoll.hpp`
* `apps/openmw/mwphysics/object.cpp`: dynamic bodies
* `apps/openmw/mwphysics/mtphysics.cpp`: dynamic object bodies
* `apps/openmw/mwphysics/physicssystem.cpp`: `moveDynamicObjects`
* `apps/openmw/mwrender/animation.cpp`: `getBoneWorldMatrix`, `setBoneWorldMatrix`, `stopAllAnimations`

## Dark Messiah importer

The importer (`tools/dm-import`) downloads these libraries at pinned commits
(`tools/dm-import/dependencies.json`); their licenses are shipped with them:

* [SourceIO](https://github.com/REDxEYE/SourceIO) by REDxEYE, MIT: reads Dark Messiah models.
* [io_scene_mw](https://github.com/Greatness7/io_scene_mw) by Greatness7, GPLv3: the `es3` library
  that reads and writes Morrowind NIF and KF files.

It runs on a standalone CPython build from
[python-build-standalone](https://github.com/astral-sh/python-build-standalone) (Python Software
Foundation License) with [NumPy](https://numpy.org) (BSD) and [Pillow](https://python-pillow.org)
(MIT-CMU).

## Game data

* Dark Messiah's animations, sounds and textures are not included; the importer converts them from
  the user's own copy of the game.
