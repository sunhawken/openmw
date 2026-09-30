#!/bin/bash
# cap.sh DIR N [crop]  -- capture N frames
export DISPLAY=:99; D=$1; N=$2; C=${3:-800x600+240+60}; mkdir -p $D
for i in $(seq -w 1 $N); do import -window root -crop $C +repage $D/f$i.png; done
