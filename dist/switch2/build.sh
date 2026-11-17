#!/usr/bin/env bash
# Builds the Switch 2 emulator core (bin/switch2/cemu_core.nro) with ounce-devkit.
set -euo pipefail

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
SWITCH2_BUILD_DIR="${SWITCH2_BUILD_DIR:-build_switch2}"
CEMU_RUNTIME_OUTPUT_DIR="${CEMU_RUNTIME_OUTPUT_DIR:-${ROOT}/bin/switch2}"
RELEASE_VERSION="${RELEASE_VERSION:-1.3.2}"
case "${BUILD_JOBS}" in
	''|*[!0-9]*|0) echo "BUILD_JOBS must be a positive integer" >&2; exit 2 ;;
esac
if [[ ! "$RELEASE_VERSION" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
	echo "RELEASE_VERSION must use major.minor.patch format" >&2
	exit 2
fi
VERSION_MAJOR="${BASH_REMATCH[1]}"
VERSION_MINOR="${BASH_REMATCH[2]}"
VERSION_PATCH="${BASH_REMATCH[3]}"
cd "${ROOT}"

if [[ ! -f "${DEVKITPRO}/libounce/cmake/Switch2.cmake" ]]; then
	echo "ounce-devkit is not installed in ${DEVKITPRO}." >&2
	echo "See https://github.com/ouncebrew/ounce-devkit (sh install.sh)." >&2
	exit 2
fi
echo ">> ounce-devkit $(cat "${DEVKITPRO}/ounce-devkit-version.txt" 2>/dev/null || echo unknown)"

bash "${ROOT}/dist/switch/deps/prepare_submodules.sh"
bash "${ROOT}/dist/switch/deps/build_boost.sh"

cmake -S . -B "${SWITCH2_BUILD_DIR}" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/Toolchain-Switch2.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCEMU_RUNTIME_OUTPUT_DIR="${CEMU_RUNTIME_OUTPUT_DIR}" \
	-DEMULATOR_VERSION_MAJOR="${VERSION_MAJOR}" \
	-DEMULATOR_VERSION_MINOR="${VERSION_MINOR}" \
	-DEMULATOR_VERSION_PATCH="${VERSION_PATCH}" \
	-DSWITCH_LTO_JOBS="${BUILD_JOBS}"

cmake --build "${SWITCH2_BUILD_DIR}" --parallel "${BUILD_JOBS}" --target CemuNro

echo ">> Output: ${CEMU_RUNTIME_OUTPUT_DIR}/cemu_core.nro"
