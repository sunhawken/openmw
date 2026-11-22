# Build environment for the Brutal Morrowind Linux package: Debian 13 with its
# GCC and Qt 6.8, plus SDL2, OpenAL, MyGUI, the OpenMW OpenSceneGraph
# fork and FFmpeg built at pinned versions (build-deps.sh, into /opt/deps).
# Used locally with podman and by GitHub Actions.
FROM docker.io/library/debian:13
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
 build-essential ccache cmake ninja-build git ca-certificates curl file unzip xz-utils pkg-config python3 nasm patchelf \
 libboost-program-options-dev libboost-system-dev libboost-iostreams-dev libboost-filesystem-dev \
 liblz4-dev libfreetype-dev libpng-dev libjpeg-dev libluajit-5.1-dev libsqlite3-dev libicu-dev \
 libyaml-cpp-dev libcollada-dom-dev zlib1g-dev libunshield-dev libgl-dev libegl-dev libgles-dev \
 qt6-base-dev qt6-base-dev-tools qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools qt6-svg-dev \
 qt6-wayland qt6-gtk-platformtheme libqt6sql6-sqlite \
 libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev \
 libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libpulse-dev libasound2-dev libudev-dev \
 libmp3lame-dev libopus-dev libvorbis-dev libvpx-dev \
 && rm -rf /var/lib/apt/lists/*
COPY build-deps.sh /usr/local/bin/build-deps.sh
RUN bash /usr/local/bin/build-deps.sh && echo /opt/deps/lib > /etc/ld.so.conf.d/deps.conf && ldconfig
# Qt SVG plugins (icons and images in the launcher).
RUN apt-get update && apt-get install -y --no-install-recommends qt6-svg-plugins && rm -rf /var/lib/apt/lists/*
WORKDIR /src
