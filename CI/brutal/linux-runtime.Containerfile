ARG BASE_IMAGE=docker.io/library/ubuntu:24.04
FROM ${BASE_IMAGE}
# Desktop runtime dependencies only: no Qt, Boost or build toolchain.
RUN if command -v apt-get >/dev/null; then \
      apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        python3 binutils libstdc++6 libgl1 libegl1 libopengl0 libx11-xcb1 libxcursor1 \
        libxrandr2 libxi6 libxss1 libxinerama1 libsm6 libice6 libxcb-cursor0 libxkbcommon-x11-0 \
        libwayland-client0 libwayland-cursor0 libwayland-egl1 libdbus-1-3 libudev1 zlib1g \
        fontconfig fonts-dejavu-core xvfb xauth xdotool weston \
      && rm -rf /var/lib/apt/lists/*; \
    else \
      dnf -y --disable-repo=fedora-cisco-openh264 --setopt=install_weak_deps=False install \
        python3 binutils libstdc++ mesa-libGL mesa-libEGL libglvnd-opengl libXcursor libXrandr \
        libXi libXScrnSaver libXinerama libxkbcommon-x11 xcb-util-cursor libSM dbus-libs \
        systemd-libs libwayland-client libwayland-cursor libwayland-egl fontconfig dejavu-sans-fonts \
        xorg-x11-server-Xvfb xdotool weston \
      && dnf clean all; \
    fi
ENV LANG=C.UTF-8
COPY check-linux-package.py smoke-linux-package.py /tests/
CMD ["python3", "/tests/smoke-linux-package.py", "/package with spaces"]
