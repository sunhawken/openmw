#!/bin/bash
# waits for game, applies console setup: collision off, clear weather, noon, Siff gear, female
export DISPLAY=:99; cd /home/user
for i in $(seq 1 80); do grep -q "VT_PHASE" omw-out-run2.log && break; sleep 3; done; sleep 4
W=$(xdotool search --name OpenMW | head -1); xdotool windowfocus $W
con(){ xdotool type --delay 30 "$1"; xdotool key Return; sleep 1.2; }
xdotool key grave; sleep 1; con "tcl"; con "fw 0"; con "set gamehour to 12"
for i in siff_fullbody_armor siff_regalia siff_jewelry; do con "player->additem $i 1"; con "player->equip $i"; done
con "enableracemenu"; xdotool key grave; sleep 4; xdotool mousemove 565 451 click 1; sleep 3; xdotool mousemove 924 553 click 1; sleep 3
xdotool key grave; sleep 1; xdotool key grave  # ensure console closed
sleep 1; import -window root omw-out/s_prep.png
