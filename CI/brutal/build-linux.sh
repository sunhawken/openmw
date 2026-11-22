#!/usr/bin/env bash
# Build the Brutal Morrowind Linux package inside the CI/brutal/linux.Containerfile image.
#   build-linux.sh [WORK_DIR] [OUTPUT_DIR]
# The package is a folder with the programs (<name>.x86_64 plus a shell wrapper
# <name>), lib/ with the bundled
# libraries and OSG plugins, plugins/ with the Qt plugins, resources/ and the
# local openmw.cfg.
set -euo pipefail
SRC="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(realpath -m "${1:-$SRC/build-linux}")"
OUT="$(realpath -m "${2:-$SRC/dist}")"
JOBS="${JOBS:-$(nproc)}"
DEPS=/opt/deps
mkdir -p "$WORK" "$OUT"

# Importer runtime: standalone CPython with numpy and Pillow.
python3 "$SRC/tools/dm-import/package_python.py" "$WORK/python-runtime" --cache "$WORK/cache"

# Resources and the importer are staged afresh, so files removed from the source
# do not linger in a reused build directory.
rm -rf "$WORK/build/resources" "$WORK/build/tools/dm-import/stage" "$WORK/build/dm-import"
# Compiler cache, when the caller provides one (CCACHE_DIR).
launcher=()
[[ -n "${CCACHE_DIR:-}" ]] && launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
cmake -S "$SRC" -B "$WORK/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$DEPS" "${launcher[@]}" \
    -DBUILD_OPENMW=ON -DBUILD_LAUNCHER=ON -DBUILD_WIZARD=ON -DBUILD_MWINIIMPORTER=ON -DBUILD_NAVMESHTOOL=ON \
    -DBUILD_OPENCS=OFF -DBUILD_ESSIMPORTER=OFF -DBUILD_BSATOOL=OFF -DBUILD_ESMTOOL=OFF -DBUILD_NIFTEST=OFF \
    -DBUILD_PHYSICSOBJECTTOOL=OFF -DBUILD_UNITTESTS=OFF -DBUILD_DOCS=OFF \
    -DOPENMW_USE_SYSTEM_OSG=ON -DOPENMW_USE_SYSTEM_MYGUI=ON \
    -DBRUTAL_PYTHON_RUNTIME="$WORK/python-runtime"
cmake --build "$WORK/build" --parallel "$JOBS"

bash "$SRC/CI/brutal/package-linux.sh" "$WORK" "$OUT"
