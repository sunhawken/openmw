#!/usr/bin/env bash
# Builds the libraries the Linux package bundles at pinned versions into $PREFIX
# (default /opt/deps). Everything else comes from Debian 13. Run inside
# CI/brutal/linux.Containerfile.
set -euo pipefail
PREFIX="${PREFIX:-/opt/deps}"
WORK="${WORK:-/tmp/deps}"
JOBS="${JOBS:-$(nproc)}"
mkdir -p "$WORK" "$PREFIX"
cd "$WORK"

fetch() { # url file sha256
    curl -sSLf -o "$2" "$1"
    echo "$3  $2" | sha256sum -c --quiet
}
cmake_build() { # source-dir extra-args...
    local src="$1"; shift
    cmake -S "$src" -B "$src-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" "$@"
    cmake --build "$src-build" --parallel "$JOBS"
    cmake --install "$src-build"
}

# SDL2 2.30.0. Window, input and sound backends are loaded at run time.
fetch https://github.com/libsdl-org/SDL/releases/download/release-2.30.0/SDL2-2.30.0.tar.gz \
    sdl.tar.gz 36e2e41557e0fa4a1519315c0f5958a87ccb27e25c51776beb6f1239526447b0
tar -xzf sdl.tar.gz
cmake_build SDL2-2.30.0 -DSDL_STATIC=OFF -DSDL_TEST=OFF

# OpenAL Soft 1.23.0 with PulseAudio and ALSA.
fetch https://github.com/kcat/openal-soft/archive/refs/tags/1.23.0.tar.gz \
    openal.tar.gz af2abf9cb539c9d66296a83ba63a75eb5868658d0f0b28b27c556f45e70c5231
tar -xzf openal.tar.gz
cmake_build openal-soft-1.23.0 -DALSOFT_EXAMPLES=OFF -DALSOFT_UTILS=OFF -DALSOFT_INSTALL_EXAMPLES=OFF \
    -DALSOFT_INSTALL_UTILS=OFF -DALSOFT_BACKEND_PIPEWIRE=OFF

# Jolt is fetched and built by the engine CMake configuration.

# MyGUI 3.4.3 (same archive and hash as extern/CMakeLists.txt).
fetch https://github.com/MyGUI/mygui/archive/refs/tags/MyGUI3.4.3.zip \
    mygui.zip 5dd90e8d2c069377f1690db075c9838086e540572e4d8ebdcddb6d76a25309c3
unzip -q mygui.zip
cmake_build mygui-MyGUI3.4.3 -DMYGUI_RENDERSYSTEM=1 -DMYGUI_BUILD_DEMOS=OFF -DMYGUI_BUILD_TOOLS=OFF \
    -DMYGUI_BUILD_PLUGINS=OFF -DMYGUI_DISABLE_PLUGINS=ON -DMYGUI_DONT_USE_OBSOLETE=ON

# OpenSceneGraph: the OpenMW fork at the commit pinned in extern/CMakeLists.txt,
# with the plugins the engine loads.
OSG_COMMIT=01cc2b585c8456a4ff843066b7e1a8715558289f
fetch "https://github.com/OpenMW/osg/archive/$OSG_COMMIT.zip" \
    osg.zip 3fe02f29906cf371ffc9e49d979c1b517669960f67a57269238bc40d58b651b7
unzip -q osg.zip
osg_plugins=()
for plugin in bmp dae dds freetype glsl jpeg osg png tga; do
    osg_plugins+=("-DBUILD_OSG_PLUGIN_${plugin^^}=ON")
done
cmake_build "osg-$OSG_COMMIT" -DBUILD_OSG_APPLICATIONS=OFF -DBUILD_OSG_EXAMPLES=OFF \
    -DBUILD_OSG_DEPRECATED_SERIALIZERS=OFF -DBUILD_OSG_PLUGINS_BY_DEFAULT=OFF "${osg_plugins[@]}" \
    -DOSG_USE_FLOAT_MATRIX=OFF -DOSG_USE_FLOAT_PLANE=OFF -DOSG_USE_FLOAT_QUAT=OFF \
    -DOPENGL_PROFILE=GL2 -DDYNAMIC_OPENSCENEGRAPH=ON -DDYNAMIC_OPENTHREADS=ON \
    -DCOLLADA_INCLUDE_DIR=/usr/include/collada-dom2.5 -DLIB_POSTFIX=

# FFmpeg 4.4.6.
fetch https://ffmpeg.org/releases/ffmpeg-4.4.6.tar.xz \
    ffmpeg.tar.xz 2290461f467c08ab801731ed412d8e724a5511d6c33173654bd9c1d2e25d0617
tar -xJf ffmpeg.tar.xz
(cd ffmpeg-4.4.6 && ./configure --prefix="$PREFIX" --enable-gpl --enable-libfreetype --enable-libmp3lame \
    --enable-libopus --enable-libvorbis --enable-libvpx --disable-static --enable-shared \
    --disable-programs --disable-doc && make -j"$JOBS" && make install)

rm -rf "$WORK"
