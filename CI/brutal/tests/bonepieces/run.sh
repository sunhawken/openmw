#!/usr/bin/env bash
# In-engine test: multi-piece equipment (AttachTo: nodes). Muted, isolated
# (own config and home), no screenshots; the result is read from the log.
#   run.sh <brutalmorrowind binary> <Morrowind Data Files dir>
set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BIN="$(realpath "$1")"; MW_DATA="$(realpath "$2")"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/cfg" "$WORK/home"
cat > "$WORK/cfg/openmw.cfg" <<CFG
data="$MW_DATA"
data="$HERE/data"
fallback-archive=Morrowind.bsa
content=Morrowind.esm
content=bonepieces_test.omwscripts
CFG
printf '[Video]\nresolution x = 640\nresolution y = 480\nwindow mode = 2\n' > "$WORK/cfg/settings.cfg"
if [[ -z "${DISPLAY:-}" ]]; then
    export DISPLAY=:97; Xvfb "$DISPLAY" -screen 0 640x480x24 >/dev/null 2>&1 & XPID=$!
    trap 'kill $XPID 2>/dev/null; rm -rf "$WORK"' EXIT; sleep 2
fi
HOME="$WORK/home" timeout 180 "$BIN" --config "$WORK/cfg" --skip-menu --start "Seyda Neen" --no-sound > "$WORK/log" 2>&1 || true
LOG="$WORK/log"
fail=0
check() { if grep -qE "$1" "$LOG"; then echo "PASS: $2"; else echo "FAIL: $2"; fail=1; fi; }
check '\[bonepieces-test\] equipped and drew weapon' "test weapon equipped and drawn"
check 'bonepieces.osg": moved 3 AttachTo piece' "3 pieces moved onto their bones"
check 'no bone "Bip01 No Such Bone" for an AttachTo piece' "missing bone reported, piece kept on weapon"
check 'detached 3 AttachTo piece' "pieces detached when the weapon was removed"
check '\[bonepieces-test\] finished' "test finished"
[[ $fail == 0 ]] || { grep -iE "bonepieces|AttachTo|error" "$LOG" | tail -20; exit 1; }
