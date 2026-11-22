#!/usr/bin/env bash
# Build Brutal Morrowind on Ubuntu 24.04 (e.g. a Claude Code cloud container)
# and optionally launch it headless under Xvfb to capture a screenshot.
#
#   CI/brutal/build-cloud.sh [--install-deps] [--run]
#
# Environment:
#   WORK      build root (default: <repo>/build-cloud)
#   JOBS      parallel jobs (default: nproc + 1)
#   MW_DATA   folder with Morrowind.esm/.bsa (required for --run)
#   START     cell to start in for --run (default: Balmora)
#   SHOT      screenshot output path for --run (default: $WORK/screenshot.png)
#
# Some networks block GitHub archive (.zip) downloads but allow git clones, so
# Jolt, recastnavigation and MyGUI are cloned at the versions pinned in
# extern/CMakeLists.txt and handed to FetchContent as local source dirs.
# MyGUI is built in-tree because Ubuntu 24.04 ships 3.4.2 and 3.4.3 is needed.
set -euo pipefail
SRC="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(realpath -m "${WORK:-$SRC/build-cloud}")"
JOBS="${JOBS:-$(($(nproc) + 1))}"
DEPS_SRC="$WORK/deps-src"
BUILD="$WORK/build"

install_deps=0
run=0
for arg in "$@"; do
    case "$arg" in
        --install-deps) install_deps=1 ;;
        --run) run=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if ((install_deps)); then
    apt-get update -qq
    bash "$SRC/CI/install_debian_deps.sh" gcc openmw-deps openmw-deps-dynamic
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq ninja-build xvfb xauth xdotool \
        libgl1-mesa-dri mesa-utils imagemagick
fi

# Versions pinned in extern/CMakeLists.txt.
extern="$SRC/extern/CMakeLists.txt"
jolt_tag=$(grep -o 'JoltPhysics/archive/refs/tags/[^ ]*\.zip' "$extern" | sed 's#.*/tags/##; s#\.zip$##')
recast_commit=$(grep -o 'OpenMW/recastnavigation/archive/[0-9a-f]*' "$extern" | sed 's#.*/##')
mygui_tag=$(grep -o 'MyGUI/mygui/archive/refs/tags/[^ ]*\.zip' "$extern" | sed 's#.*/tags/##; s#\.zip$##')

clone_at() { # url ref dir
    local url="$1" ref="$2" dir="$3"
    if [[ -d "$dir/.git" ]] && [[ "$(git -C "$dir" rev-parse HEAD)" == "$(git -C "$dir" rev-parse "$ref^{commit}" 2>/dev/null)" ]]; then
        return
    fi
    rm -rf "$dir"
    git init -q "$dir"
    git -C "$dir" fetch -q --depth 1 "$url" "$ref"
    git -C "$dir" checkout -q FETCH_HEAD
}
mkdir -p "$DEPS_SRC"
clone_at https://github.com/jrouwe/JoltPhysics "refs/tags/$jolt_tag" "$DEPS_SRC/joltphysics"
clone_at https://github.com/OpenMW/recastnavigation "$recast_commit" "$DEPS_SRC/recastnavigation"
clone_at https://github.com/MyGUI/mygui "refs/tags/$mygui_tag" "$DEPS_SRC/mygui"

launcher=()
command -v ccache >/dev/null && launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "${launcher[@]}" \
    -DFETCHCONTENT_SOURCE_DIR_JOLTPHYSICS="$DEPS_SRC/joltphysics" \
    -DFETCHCONTENT_SOURCE_DIR_RECASTNAVIGATION="$DEPS_SRC/recastnavigation" \
    -DOPENMW_USE_SYSTEM_MYGUI=OFF -DFETCHCONTENT_SOURCE_DIR_MYGUI="$DEPS_SRC/mygui" \
    -DBUILD_OPENMW=ON -DBUILD_BSATOOL=ON \
    -DBUILD_LAUNCHER=OFF -DBUILD_WIZARD=OFF -DBUILD_MWINIIMPORTER=OFF -DBUILD_NAVMESHTOOL=OFF \
    -DBUILD_OPENCS=OFF -DBUILD_ESSIMPORTER=OFF -DBUILD_ESMTOOL=OFF -DBUILD_NIFTEST=OFF \
    -DBUILD_PHYSICSOBJECTTOOL=OFF -DBUILD_RESUMELAUNCHER=OFF -DBUILD_DM_IMPORT=OFF -DBUILD_UNITTESTS=OFF
cmake --build "$BUILD" --parallel "$JOBS" --target openmw bsatool

((run)) || exit 0

# ---- Headless run -----------------------------------------------------------
: "${MW_DATA:?set MW_DATA to the folder with Morrowind.esm and Morrowind.bsa}"
MW_DATA="$(realpath "$MW_DATA")"
START="${START:-Balmora}"
SHOT="$(realpath -m "${SHOT:-$WORK/screenshot.png}")"
CFG="$WORK/cfg"
mkdir -p "$CFG" "$WORK/home"

# User openmw.cfg: data root, BSA archives and content files that exist.
{
    echo "data=\"$MW_DATA\""
    for name in Morrowind Tribunal Bloodmoon; do
        [[ -f "$MW_DATA/$name.bsa" ]] && echo "fallback-archive=$name.bsa"
    done
    for name in Morrowind Tribunal Bloodmoon; do
        [[ -f "$MW_DATA/$name.esm" ]] && echo "content=$name.esm"
    done
} > "$CFG/openmw.cfg"
[[ -f "$CFG/settings.cfg" ]] || printf '[Video]\nresolution x = 1280\nresolution y = 720\nwindow mode = 2\n' > "$CFG/settings.cfg"
# Console commands run at startup. The default --start spot for Balmora faces
# a rock wall, so move the player into town for a useful screenshot.
if [[ ! -f "$CFG/start.txt" ]]; then
    if [[ "$START" == Balmora ]]; then
        echo 'player->position -21000 -12000 1500 0' > "$CFG/start.txt"
    else
        : > "$CFG/start.txt"
    fi
fi

if [[ -z "${DISPLAY:-}" ]]; then
    export DISPLAY=:99
    Xvfb "$DISPLAY" -screen 0 1280x720x24 +extension GLX >/dev/null 2>&1 &
    xvfb_pid=$!
    trap 'kill $xvfb_pid 2>/dev/null || true' EXIT
    sleep 2
fi

HOME="$WORK/home" "$BUILD/brutalmorrowind" --config "$CFG" --skip-menu --start "$START" --no-sound \
    --script-run "$CFG/start.txt" > "$WORK/run.log" 2>&1 &
game_pid=$!
sleep "${WAIT:-75}"
if ! kill -0 "$game_pid" 2>/dev/null; then
    echo "game exited early; see $WORK/run.log" >&2
    exit 1
fi
import -window root "$SHOT"
kill "$game_pid"
wait "$game_pid" 2>/dev/null || true

grep -E "Adding BSA archive|Adding data directory|OpenGL Renderer" "$WORK/run.log" || true
# Error-level log lines ("[time E]") and missing resources.
if grep -qE "^\[[^]]* E\]|[Nn]ot found|[Cc]an't find|[Ff]ailed to" "$WORK/run.log"; then
    echo "warning: errors in $WORK/run.log" >&2
fi
echo "screenshot: $SHOT"
