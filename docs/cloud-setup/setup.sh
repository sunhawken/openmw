#!/bin/bash
# Fast repeatable setup: OpenMW branch official-jolt-ragdoll on a fresh Ubuntu 24.04 cloud session.
# Needs Morrowind.esm/.bsa fetched separately (see README.md). Run: bash setup.sh
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  libboost-program-options-dev libboost-system-dev libboost-iostreams-dev libboost-filesystem-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libsdl2-dev \
  libqt5opengl5-dev qttools5-dev qttools5-dev-tools libopenal-dev libunshield-dev libtinyxml-dev \
  liblz4-dev libpng-dev libjpeg-dev libluajit-5.1-dev librecast-dev libsqlite3-dev libicu-dev \
  libyaml-cpp-dev libqt5svg5-dev libmygui-dev libopenscenegraph-dev libcollada-dom-dev \
  mesa-utils libgl1-mesa-dri libgl-dev libfreetype-dev ccache mold xvfb xdotool imagemagick
D=/home/user
# github.com/.../archive/*.zip (FetchContent) is 403 in the sandbox; git clones work, so pre-clone deps.
mkdir -p $D/jrouwe
[ -d $D/jrouwe/joltphysics ] || GIT_LFS_SKIP_SMUDGE=1 git clone -q --depth 1 -b v5.3.0 https://github.com/jrouwe/joltphysics $D/jrouwe/joltphysics
[ -d $D/jrouwe/mygui ] || git clone -q --depth 1 -b MyGUI3.4.3 https://github.com/MyGUI/mygui $D/jrouwe/mygui   # distro has 3.4.2, need 3.4.3
if [ ! -d $D/jrouwe/recast ]; then
  git clone -q https://github.com/OpenMW/recastnavigation $D/jrouwe/recast
  git -C $D/jrouwe/recast checkout -q 03259f3287ff8330f0d66fcd98d022edddffaa97   # hash from extern/CMakeLists.txt
fi
[ -d $D/openmw ] || git clone --depth 1 -b official-jolt-ragdoll https://github.com/sunhawken/openmw $D/openmw
mkdir -p $D/openmw-build && cd $D/openmw-build
ccache --max-size=20G >/dev/null
# Speed: -O1 -g0 (fast compile, fine for physics testing), unity build off by default (uses RAM), mold linker, ccache.
FAST='-O1 -g0'
cmake -G Ninja $D/openmw -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="$FAST -DNDEBUG" -DCMAKE_C_FLAGS_RELEASE="$FAST -DNDEBUG" \
  -DFETCHCONTENT_SOURCE_DIR_JOLTPHYSICS=$D/jrouwe/joltphysics \
  -DFETCHCONTENT_SOURCE_DIR_RECASTNAVIGATION=$D/jrouwe/recast \
  -DFETCHCONTENT_SOURCE_DIR_MYGUI=$D/jrouwe/mygui -DOPENMW_USE_SYSTEM_MYGUI=OFF -DMYGUI_STATIC=ON \
  -DBUILD_LAUNCHER=OFF -DBUILD_WIZARD=OFF -DBUILD_OPENCS=OFF -DBUILD_ESSIMPORTER=OFF -DBUILD_MWINIIMPORTER=OFF \
  -DBUILD_NAVMESHTOOL=OFF -DBUILD_PHYSICSOBJECTTOOL=OFF -DBUILD_BSATOOL=OFF -DBUILD_ESMTOOL=OFF -DBUILD_NIFTEST=OFF \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold
ninja -j$(nproc) openmw     # ~20-30 min on 4 cores; incremental rebuilds are fast
