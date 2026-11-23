#!/usr/bin/env bash
# In-engine test: NPCs whose model is a generated skinned Collada rig, one
# with a non-Bip01 skeleton and one with stock Bip01 names. Checks exact bone
# names, clip groups and bone motion. Muted, isolated, no screenshots.
#   run.sh <brutalmorrowind binary> <Morrowind Data Files dir>
set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BIN="$(realpath "$1")"; MW_DATA="$(realpath "$2")"
WORK="$(mktemp -d)"; XPID=""
trap '[[ -n "$XPID" ]] && kill $XPID 2>/dev/null; rm -rf "$WORK"' EXIT
mkdir -p "$WORK/cfg" "$WORK/home" "$WORK/data/meshes/test"
cp -r "$HERE/data/." "$WORK/data/"
python3 "$HERE/../fixtures/make_collada_rig.py" "$WORK/data/meshes/test" --name rig >/dev/null
python3 "$HERE/../fixtures/make_collada_rig.py" "$WORK/data/meshes/test" --name rig_bip01 --bip01 >/dev/null
python3 "$HERE/../fixtures/make_collada_rig.py" "$WORK/data/meshes/test" --name rig_legacy --bip01 --legacy >/dev/null
# Stress: RIG_STRESS extra custom rigs under distinct names, all loading at once.
STRESS="${RIG_STRESS:-0}"
MODELS="'meshes/test/rig.dae', 'meshes/test/rig_bip01.dae', 'meshes/test/rig_legacy.dae'"
for ((i = 1; i <= STRESS; i++)); do
    python3 "$HERE/../fixtures/make_collada_rig.py" "$WORK/data/meshes/test" --name "rig_s$i" >/dev/null
    MODELS="$MODELS, 'meshes/test/rig_s$i.dae'"
done
printf 'return { %s }\n' "$MODELS" > "$WORK/data/scripts/rig_models.lua"
EXPECTED=$((3 + STRESS))
cat > "$WORK/cfg/openmw.cfg" <<CFG
data="$MW_DATA"
data="$WORK/data"
fallback-archive=Morrowind.bsa
content=Morrowind.esm
content=rig_test.omwscripts
CFG
printf '[Video]\nresolution x = 640\nresolution y = 480\nwindow mode = 2\n' > "$WORK/cfg/settings.cfg"
if [[ -z "${DISPLAY:-}" ]]; then
    export DISPLAY=:95; Xvfb "$DISPLAY" -screen 0 640x480x24 >/dev/null 2>&1 & XPID=$!; sleep 2
fi
HOME="$WORK/home" timeout "${RIG_TEST_TIMEOUT:-60}" "$BIN" --config "$WORK/cfg" --skip-menu --start "Seyda Neen" --no-sound > "$WORK/log" 2>&1 || true
cp "$WORK/log" "${RIG_TEST_LOG:-/dev/null}" 2>/dev/null || true
grep -E "\[rig-test\]" "$WORK/log" | sed 's/.*\[rig-test\] //' || true
# Every rig: two skinned meshes and three clips, from the loader's own summary.
SKINS_OK=$(grep -cE "Collada model meshes/test/rig[a-z0-9_]*\.dae: 2 skinned meshes, 3 clips, skeleton found" "$WORK/log" || true)
LOAD_FAIL=$(grep -c "Failed to load 'meshes/test/rig" "$WORK/log" || true)
echo "loader summary: $SKINS_OK/$EXPECTED rigs with 2 skins and 3 clips; $LOAD_FAIL load failures"
if [[ "$SKINS_OK" -ne "$EXPECTED" || "$LOAD_FAIL" -ne 0 ]]; then echo "RESULT: FAIL"; exit 1; fi
if grep -q "\[rig-test\].* FAIL" "$WORK/log" || [[ $(grep -c "\[rig-test\].* finished" "$WORK/log") -lt $EXPECTED ]]; then
    echo "RESULT: FAIL"; exit 1
fi
echo "RESULT: PASS"
