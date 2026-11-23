#!/usr/bin/env bash
# In-engine test: move set engine layer (input path, animation, sound, effect,
# movement, hits, listeners). Muted, isolated config and home, no screenshots.
#   run.sh <brutalmorrowind binary> <Morrowind Data Files dir>
set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BIN="$(realpath "$1")"; MW_DATA="$(realpath "$2")"
WORK="$(mktemp -d)"; XPID=""
trap '[[ -n "$XPID" ]] && kill $XPID 2>/dev/null; rm -rf "$WORK"' EXIT
mkdir -p "$WORK/cfg" "$WORK/home"
cat > "$WORK/cfg/openmw.cfg" <<CFG
data="$MW_DATA"
data="$HERE/data"
fallback-archive=Morrowind.bsa
content=Morrowind.esm
content=moveset_test.omwscripts
CFG
printf '[Video]\nresolution x = 640\nresolution y = 480\nwindow mode = 2\n' > "$WORK/cfg/settings.cfg"
if [[ -z "${DISPLAY:-}" ]]; then
    export DISPLAY=:96; Xvfb "$DISPLAY" -screen 0 640x480x24 >/dev/null 2>&1 & XPID=$!; sleep 2
fi
HOME="$WORK/home" timeout 180 "$BIN" --config "$WORK/cfg" --skip-menu --start "Seyda Neen" --no-sound > "$WORK/log" 2>&1 || true
grep -E "\[moveset-test\]|\[BrutalMoveset\]" "$WORK/log" || true
if grep -q "\[moveset-test\] FAIL" "$WORK/log" || ! grep -q "\[moveset-test\] finished" "$WORK/log"; then
    echo "RESULT: FAIL"; grep -iE "error|lua" "$WORK/log" | tail -15; exit 1
fi
echo "RESULT: PASS"
