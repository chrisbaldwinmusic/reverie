#!/bin/sh
# Builds Reverie (AU, VST3, Standalone), runs the offline checks, and installs the
# plugins into ~/Library/Audio/Plug-Ins. Needs cmake + ninja (pip3 install --user cmake ninja).
#
#   ./build.sh                build, check, install, then validate the plug-ins
#   ./build.sh --no-install   build and check only (the VST3 is still validated from the build folder)
#   ./build.sh --quick        skip pluginval and auval
#
# Validation uses pluginval (fetched once into ~/Library/Caches, pinned to a release) at its
# strictest level, which loads the plug-in the way a host does and checks state round trips,
# automation from any thread, odd block sizes and sample rates, and the editor. Once installed,
# the AU also goes through Apple's auval, as Logic will do.
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
PLUGINVAL_TAG="v1.0.4"
INSTALL=ON
VALIDATE=ON
for arg in "$@"; do
    case "$arg" in
        --no-install) INSTALL=OFF ;;
        --quick)      VALIDATE=OFF ;;
        *) echo "unknown option: $arg"; exit 1 ;;
    esac
done

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

[ "$VALIDATE" = OFF ] && exit 0

PV_DIR="$HOME/Library/Caches/pluginval-$PLUGINVAL_TAG"
PV="$PV_DIR/pluginval.app/Contents/MacOS/pluginval"
if [ ! -x "$PV" ]; then
    mkdir -p "$PV_DIR"
    curl -fsSL -o "$PV_DIR/pluginval.zip" "https://github.com/Tracktion/pluginval/releases/download/$PLUGINVAL_TAG/pluginval_macOS.zip"
    unzip -q -o "$PV_DIR/pluginval.zip" -d "$PV_DIR"
    rm "$PV_DIR/pluginval.zip"
fi

if [ "$INSTALL" = ON ]; then
    VST3="$HOME/Library/Audio/Plug-Ins/VST3/Reverie.vst3"
    AU="$HOME/Library/Audio/Plug-Ins/Components/Reverie.component"
else
    VST3="$BUILD/Reverie_artefacts/Release/VST3/Reverie.vst3"
    AU=""
fi

LOGS="$BUILD/validation"
mkdir -p "$LOGS"
echo "pluginval (strictness 10): VST3"
"$PV" --strictness-level 10 --timeout-ms 600000 --output-dir "$LOGS" --validate "$VST3" > "$LOGS/vst3.txt" 2>&1 \
    || { tail -40 "$LOGS/vst3.txt"; echo "pluginval FAILED for the VST3 (full log: $LOGS/vst3.txt)"; exit 1; }
echo "  passed"

if [ -n "$AU" ]; then
    killall -9 AudioComponentRegistrar 2>/dev/null || true      # so the freshly installed AU is the one found
    echo "pluginval (strictness 10): AU"
    "$PV" --strictness-level 10 --timeout-ms 600000 --output-dir "$LOGS" --validate "$AU" > "$LOGS/au.txt" 2>&1 \
        || { tail -40 "$LOGS/au.txt"; echo "pluginval FAILED for the AU (full log: $LOGS/au.txt)"; exit 1; }
    echo "  passed"
    echo "auval"
    auval -v aufx Rvre Chnl > "$LOGS/auval.txt" 2>&1 \
        || { tail -40 "$LOGS/auval.txt"; echo "auval FAILED (full log: $LOGS/auval.txt)"; exit 1; }
    echo "  passed"
fi
