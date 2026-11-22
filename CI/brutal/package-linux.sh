#!/usr/bin/env bash
# Package an existing Linux build using the dependencies in the build image.
#   package-linux.sh WORK_DIR OUTPUT_DIR
set -euo pipefail
SRC="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(realpath "$1")"
OUT="$(realpath -m "$2")"
DEPS=/opt/deps
VERSION="$(sed -n 's/^set(BRUTAL_VERSION_\(MAJOR\|MINOR\|RELEASE\) \([0-9]*\))$/\2/p' "$SRC/CMakeLists.txt" | paste -sd.)"
NAME="BrutalMorrowind-$VERSION-Linux-64Bit"
mkdir -p "$OUT"

PKG="$WORK/$NAME"
rm -rf "$PKG"
mkdir -p "$PKG/lib/osgPlugins-3.6.5" "$PKG/plugins" "$PKG/desktop files"

# Programs and their wrappers.
QT_PROGRAMS=(brutalmorrowind-launcher brutalmorrowind-wizard)
PROGRAMS=(brutalmorrowind brutalmorrowind-iniimporter brutalmorrowind-navmeshtool "${QT_PROGRAMS[@]}")
wrapper() { # program extra-environment
    cat > "$PKG/$1" <<EOF
#!/bin/sh
# Runs $1.x86_64 with the libraries shipped in ./lib.
SCRIPT="\$0"
while [ -L "\$SCRIPT" ]; do
    LINK=\$(ls -l "\$SCRIPT" | sed 's/^.*-> //')
    case "\$LINK" in
        /*) SCRIPT="\$LINK" ;;
        *) SCRIPT="\$(dirname "\$SCRIPT")/\$LINK" ;;
    esac
done
cd "\$(dirname "\$SCRIPT")" || { echo "Failed to enter \$(dirname "\$SCRIPT")"; exit 1; }
$2
if [ -n "\$DEBUG" ]; then
    command -v gdb >/dev/null 2>&1 || { echo "Could not find gdb"; exit 1; }
    export DEBUGINFOD_URLS=""
    exec gdb --args ./$1.x86_64 "\$@"
fi
exec ./$1.x86_64 "\$@"
EOF
    chmod 755 "$PKG/$1"
}
for program in "${PROGRAMS[@]}"; do
    cp "$WORK/build/$program" "$PKG/$program.x86_64"
    environment='export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"'
    [[ " ${QT_PROGRAMS[*]} " == *" $program "* ]] && environment+=$'\nunset QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH'
    wrapper "$program" "$environment"
done
# The importer runs its own Python, which must not see the game's libraries.
cp "$WORK/build/brutalmorrowind-import" "$PKG/brutalmorrowind-import.x86_64"
wrapper brutalmorrowind-import 'unset LD_LIBRARY_PATH QT_PLUGIN_PATH'
cp -r "$WORK/build/dm-import" "$PKG/dm-import"
rm -f "$PKG/dm-import/python/.copied"
# The MDL readers use SourceIO's Python modules. Its unused Blender native
# extension has a separate system ABI and is not part of this importer.
rm -rf "$PKG/dm-import/third_party/SourceIO/library/utils/pylib"
cat > "$PKG/qt.conf" <<'EOF'
[Paths]
Prefix=.
Plugins=plugins
EOF

cp "$WORK/build/openmw.cfg" "$WORK/build/defaults.bin" "$WORK/build/gamecontrollerdb.txt" "$PKG/"
cp -r "$WORK/build/resources" "$PKG/"
cp "$SRC/files/launcher/images/openmw.png" "$PKG/brutalmorrowind.png"
cp "$WORK/build/brutalmorrowind.desktop" "$PKG/desktop files/"
cp "$SRC/README.md" "$SRC/LICENSE" "$SRC/CHANGELOG.md" "$SRC/AUTHORS.md" "$PKG/"
[[ -f "$SRC/CREDITS.md" ]] && cp "$SRC/CREDITS.md" "$PKG/"
cp "$SRC/CI/brutal/README.packagecontents.txt" "$PKG/"

# OSG plugins the engine loads.
for plugin in bmp dae dds freetype glsl jpeg osg png serializers_osg tga; do
    cp "$DEPS/lib/osgPlugins-3.6.5/osgdb_$plugin.so" "$PKG/lib/osgPlugins-3.6.5/"
done

# Desktop Qt plugins. Embedded display backends and compositor server plugins
# are not used by the launcher or wizard.
QT_PLUGINS=/usr/lib/x86_64-linux-gnu/qt6/plugins
for folder in iconengines imageformats platforminputcontexts \
    platforms platformthemes wayland-decoration-client \
    wayland-graphics-integration-client wayland-shell-integration \
    xcbglintegrations; do
    [[ -d "$QT_PLUGINS/$folder" ]] && cp -r "$QT_PLUGINS/$folder" "$PKG/plugins/"
done
rm -f "$PKG/plugins/platforms/libqeglfs.so" "$PKG/plugins/platforms/libqlinuxfb.so" \
    "$PKG/plugins/platforms/libqvkkhrdisplay.so" \
    "$PKG/plugins/platformthemes/libqgtk3.so"

# Libraries: everything the programs, the OSG plugins and the Qt plugins for
# windows, input, images and the Wayland client load, except what is left to
# the system: the C/C++ runtime, zlib, graphics drivers,
# X11/xcb, Wayland, xkbcommon, fontconfig and desktop services. Every shipped plugin is
# included in the dependency walk, including VNC and Wayland helpers.
SYSTEM='^(ld-linux.*|lib(c|m|dl|pthread|rt|util|resolv|stdc\+\+|gcc_s|z)\.so.*|lib(GL|GLX|OpenGL|EGL|GLdispatch|GLESv2|gbm|drm)\.so.*|libX[^/]*\.so.*|libxcb(-(glx|dri2|dri3|present))?\.so.*|libSM\.so.*|libICE\.so.*|libwayland-[^/]*\.so.*|libxkbcommon[^/]*\.so.*|libdbus-1\.so.*|libudev\.so.*|libsystemd\.so.*|libinput\.so.*|libmtdev\.so.*|libfontconfig\.so.*)$'
mapfile -t elves < <(find "$PKG" -maxdepth 1 -type f -name '*.x86_64'
    find "$PKG/lib/osgPlugins-3.6.5" -type f -name '*.so'
    find "$PKG/plugins" -type f -name '*.so')
LD_LIBRARY_PATH="$PKG/lib:$DEPS/lib" python3 "$SRC/CI/brutal/bundle_libs.py" "$PKG/lib" "$SYSTEM" "${elves[@]}"
# Standalone Python keeps a separate library directory and search paths.
mapfile -t python_elves < <(find "$PKG/dm-import/python" -type f \( -name '*.so' -o -name '*.so.*' \))
LD_LIBRARY_PATH="$PKG/dm-import/python/lib" python3 "$SRC/CI/brutal/bundle_libs.py" --preserve-internal \
    "$PKG/dm-import/python/lib" "$SYSTEM" "$PKG/dm-import/python/bin/python3" "${python_elves[@]}"
python3 "$SRC/CI/brutal/relocate-linux.py" "$PKG"
python3 "$SRC/CI/brutal/check-linux-package.py" "$PKG"

tar -C "$WORK" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "Package: $OUT/$NAME.tar.gz"
