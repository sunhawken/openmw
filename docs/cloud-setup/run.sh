#!/bin/bash
# Launch OpenMW headless under Xvfb (software GL). Usage: run.sh [extra openmw args]
D=/home/user
mkdir -p $D/omw-config $D/omw-out
cat > $D/omw-config/openmw.cfg <<CFG
data="$D/morrowind-data"
content=Morrowind.esm
fallback-archive=Morrowind.bsa
CFG
pkill Xvfb 2>/dev/null; Xvfb :99 -screen 0 1280x720x24 & sleep 2
export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 OPENMW_CONFIG_DIR=$D/omw-config
cd $D/openmw-build
exec ./openmw --config=$D/omw-config --user-data=$D/omw-out --skip-menu --start "Seyda Neen, Census and Excise Office" --no-grab "$@"
