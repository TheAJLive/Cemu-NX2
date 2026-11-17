#!/bin/bash
# Builds the Switch 2 all-in-one launcher (switch_launcher/switch2/cemu.nro):
# the Vulkan/OpenGL core built with ounce-devkit, embedded in the SDL launcher.
set -euo pipefail

BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
RELEASE_VERSION="${RELEASE_VERSION:-1.3.2}"
case "${BUILD_JOBS}" in
	''|*[!0-9]*|0) echo "BUILD_JOBS must be a positive integer" >&2; exit 2 ;;
esac
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="${DEVKITPRO}/tools/bin:${DEVKITPRO}/devkitA64/bin:$PATH"

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

echo "[1/3] Building the emulator core"
RELEASE_VERSION="$RELEASE_VERSION" BUILD_JOBS="$BUILD_JOBS" bash "$ROOT/dist/switch2/build.sh"

echo "[2/3] Staging the core"
STAGE="$ROOT/switch_launcher/romfs_switch2"
rm -rf "$STAGE"
mkdir -p "$STAGE/emu"
cp -f "$ROOT/switch_launcher/romfs/logo.png" "$STAGE/logo.png"
cp -f "$ROOT/switch_launcher/Cemu.jpg" "$STAGE/forwarder-icon.jpg"
cp -f "$ROOT/bin/switch2/cemu_core.nro" "$STAGE/emu/cemu_core.nro"
sha256sum "$STAGE/emu/cemu_core.nro" | awk '{print $1}' > "$STAGE/emu/cemu_core.sha256"

echo "[3/3] Building the launcher"
cd "$ROOT/switch_launcher"
make SWITCH2=1 RELEASE_VERSION="$RELEASE_VERSION" APP_VERSION="$RELEASE_VERSION" clean
make -j "$BUILD_JOBS" SWITCH2=1 RELEASE_VERSION="$RELEASE_VERSION" APP_VERSION="$RELEASE_VERSION"

echo
echo "Copy switch_launcher/switch2/cemu.nro anywhere under sdmc:/switch/"
ls -la "$ROOT/switch_launcher/switch2/cemu.nro"
