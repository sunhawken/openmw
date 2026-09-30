#!/bin/bash
# shoot.sh OUTDIR : restart game, prep, capture one full cycle (~ 18s game) starting at idle
export DISPLAY=:99; cd /home/user; OUT=$1; rm -rf $OUT
pkill openmw; sleep 2; nohup ./run2.sh > omw-out-run2.log 2>&1 &
./prep.sh
for k in 1 2 3; do a=$(grep -c VT_PHASE omw-out-run2.log); sleep 5; b=$(grep -c VT_PHASE omw-out-run2.log); [ $a -ne $b ] && break; xdotool key grave; sleep 2; done
n=$(grep -c "VT_PHASE idle" omw-out-run2.log)
for i in $(seq 1 100); do [ $(grep -c "VT_PHASE idle" omw-out-run2.log) -gt $n ] && break; sleep 0.3; done
./cap.sh $OUT 34 800x520+240+80
grep VT_PHASE omw-out-run2.log | tail -5
