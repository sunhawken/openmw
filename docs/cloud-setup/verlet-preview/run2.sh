#!/bin/bash
D=/home/user
mkdir -p $D/omw-config $D/omw-out
cat > $D/omw-config/openmw.cfg <<CFG
data="$D/morrowind-data"
data="$D/siff-data"
data="$D/vt-data"
content=Morrowind.esm
content=Siff.esp
content=verlettest.omwscripts
fallback-archive=Morrowind.bsa
CFG
pgrep Xvfb >/dev/null || { Xvfb :99 -screen 0 1280x720x24 & sleep 2; }
export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 OPENMW_CONFIG_DIR=$D/omw-config
cd $D/openmw-build
exec ./openmw --config=$D/omw-config --user-data=$D/omw-out --skip-menu --start "${START:-Seyda Neen}" --no-grab "$@"
