#!/bin/sh -ex

brew tap --repair
brew update --quiet

brew install curl p7zip

VCPKG_TAG="2026-02-24"

if [[ "${MACOS_AMD64}" ]]; then
    VCPKG_FILE="vcpkg-x64-osx-dynamic"
    command -v /usr/local/bin/brew || arch -x86_64 bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

    arch -x86_64 bash -c "command -v qmake >/dev/null 2>&1 && qmake -v | grep -F 'Using Qt version 6.' >/dev/null || /usr/local/bin/brew install qt@6"
    arch -x86_64 /usr/local/bin/brew install fontconfig yaml-cpp sqlite
else
    VCPKG_FILE="vcpkg-arm64-osx-dynamic"

    command -v qmake >/dev/null 2>&1 && qmake -v | grep -F "Using Qt version 6." >/dev/null || brew install qt@6
    brew install fontconfig yaml-cpp sqlite
fi

curl "https://gitlab.com/OpenMW/openmw-deps/-/raw/vcpkg-8306cb2556b92c9bb891b13f4769889838fe3c10/macos/${VCPKG_FILE}-${VCPKG_TAG}-manifest.txt" -o openmw-manifest.txt

{ read -r URL && read -r HASH FILE; } < openmw-manifest.txt

curl -fSL -R -J $URL -o $FILE
echo "${HASH:?}  ${FILE:?}" | sha512sum
7z x -y -o/tmp/openmw-deps-pre -spe $FILE && \
    mv /tmp/openmw-deps-pre/*/ /tmp/openmw-deps/ && \
    rmdir /tmp/openmw-deps-pre

command -v cmake >/dev/null 2>&1 || brew install cmake
