# Siff directional cloth regression checks

These checks execute the real VerletClothController C++ source using a minimal
scene/matrix adapter. They use the full Siff skeleton, vertices, faces, weights,
and bind matrices extracted from the supplied NIFs. They are headless numerical
checks, not OpenMW gameplay or a replacement for an OpenMW build.

Prerequisites: C++17 compiler, Python, NumPy, SciPy, and PyFFI (legacy PyFFI is
compatible with the included time.clock shim). From the repository root:

```sh
python3 scripts/siff_verlet_tests/extract_assets.py --meshes /absolute/path/to/Meshes/Siff
python3 scripts/siff_verlet_tests/build.py
python3 scripts/siff_verlet_tests/verify_final.py
```

The suite checks 72 cases per asset: walking, running, and jumping in eight
compass directions; 26 impulse directions; stress at 20, 30, 60, 120, and 144 fps;
variable timing, pauses, teleport, rewind, settle and wake; and sixteen paired
steady-motion drag checks. Drag checks compare identical movement with drag on
and off, including release after stopping and stronger running than walking.

Full-resolution skin checks sample 135 poses per asset across all compass
movements and backward/stress sequences. The evaluator agrees with PyFFI's rest
skinning within 9e-14 units. Chain centerline ring widths remain diagnostic:
they are not surface vertices. The actual skinned surface bands determine the
mesh-width check. The adapter omits the renderer, actual game animations,
terrain queries, and the full OSG/runtime integration.

The JSON report records settings, acceptance thresholds, every case, and all
measured minimum/maximum values. A full OpenMW build and gameplay inspection are
still required. The bridge's USE_BASELINE mode is optional and requires the
separate previous-source snapshots under original/; the standard suite uses
only the current controller.

Validated Siff settings (2026-09-29): hair minimum sustained running trail
8.07 units, skirt 5.18 units, across all eight motion directions. Maximum
segment stretch was 2.43% and 4.07%; minimum measured skinned surface-band
width was 72.38% and 80.00%. Quiet long-settle movement was zero, and both
meshes woke on renewed movement. These are numerical test results, not
gameplay observations.
