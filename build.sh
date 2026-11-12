#!/bin/sh
# Builds Reverie (AU, VST3, Standalone), runs the offline checks, and installs the
# plugins into ~/Library/Audio/Plug-Ins. Needs cmake + ninja (pip3 install --user cmake ninja).
#
#   ./build.sh                build, check, install
#   ./build.sh --no-install   build and check only
#
# JUCE is not stored in this repository. The script uses, in order: a JUCE folder here,
# the copy in the Atelier project next door (../Channel/JUCE), or a copy it fetches
# itself (pinned to a tag) into ~/Library/Caches.
#
# The build directory lives outside ~/Documents on purpose: a cloud-synced Documents
# folder makes the thousands of small build files slow and occasionally time out.
set -e
cd "$(dirname "$0")"
export PATH="$HOME/Library/Python/3.9/bin:$PATH"
BUILD="$HOME/Library/Caches/Reverie-build"
JUCE_TAG="9.0.5"
INSTALL=ON
[ "$1" = "--no-install" ] && INSTALL=OFF

if [ -f JUCE/CMakeLists.txt ]; then
    JUCE="$PWD/JUCE"
elif [ -f ../Channel/JUCE/CMakeLists.txt ]; then
    JUCE="$PWD/../Channel/JUCE"
else
    JUCE="$HOME/Library/Caches/Reverie-JUCE-$JUCE_TAG"
    [ -f "$JUCE/CMakeLists.txt" ] || git clone --depth 1 --branch "$JUCE_TAG" https://github.com/juce-framework/JUCE.git "$JUCE"
fi

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DREVERIE_INSTALL_PLUGINS=$INSTALL -DREVERIE_JUCE_DIR="$JUCE"
cmake --build "$BUILD" --target ReverieTests
"$BUILD/ReverieTests_artefacts/Release/ReverieTests" "$BUILD/shots"
cmake --build "$BUILD"
echo "Standalone app: $BUILD/Reverie_artefacts/Release/Standalone/Reverie.app"
