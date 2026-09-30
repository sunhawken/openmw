# Siff directional cloth regression checks

These checks execute the real controller C++ with a minimal scene/matrix adapter
and full Siff skeleton/skin data extracted from NIFs. They are headless numerical
checks, not gameplay or a replacement for a full OpenMW build.

Requires C++17 compiler, Python, NumPy, SciPy, PyFFI and Pillow for previews.
The extraction script provides the time.clock compatibility shim for legacy PyFFI.
From the repository root:

```sh
python3 scripts/siff_verlet_tests/extract_assets.py --meshes /absolute/path/to/Meshes/Siff
python3 scripts/siff_verlet_tests/build.py
python3 scripts/siff_verlet_tests/verify_final.py
python3 scripts/siff_verlet_tests/render_full_speed.py
```

Optional `build.py --output NAME.so`, `verify_final.py --library NAME.so
--output-dir PATH`, and `render_full_speed.py --library NAME.so` support isolated
comparisons. Fixture files remain in the script directory. Preview GIFs/PNGs
are written to its previews directory.

The suite checks 80 cases per model: eight directions of walking/running/jumping;
26 impulse directions; stress at 20/30/60/120/144 fps; timing/pause/teleport/rewind/
settle/wake; 16 paired drag-on/off speed/release cases; and eight continuous
full-speed runs at 320 units/s. Those runs check root tilt and tip lift across
every frame of the 1.8–5.8 s cruise. Full-resolution skin checks cover 167 poses
per model, including all compass movements and full-speed/backward/stress poses.

The report includes settings, opt-ins, source and adapter hashes, all acceptance
limits, and the worst surface-band sample. Centerline ring width is diagnostic;
the actual skinned surface defines the mesh-width check. The evaluator agrees
with PyFFI's rest skinning within 9e-14 units.

Validated results: minimum full-speed median root tilt 57.38 degrees
for hair and 49.05 for skirt; minimum sampled mesh-band
width retained 71.84% and 83.74%; maximum mean residual drag offset
after stopping 0.712 units. Long quiet settle movement is zero and wake passes.

Actual game animations, terrain and full OSG/runtime integration are absent.
Ground uses the existing actor-root plane. Full builds/gameplay remain required.
The optional USE_BASELINE bridge mode needs separate old sources under original/.
