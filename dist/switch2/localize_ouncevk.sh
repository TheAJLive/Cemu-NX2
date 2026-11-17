#!/usr/bin/env bash
# Localize OunceVK's public Vulkan and GL entry points for Cemu's dispatch
# tables. Usage: localize_ouncevk.sh <output-dir>
set -euo pipefail

OUT="${1:?usage: $0 <output-dir>}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
BIN="${DEVKITPRO}/devkitA64/bin"
NM="${BIN}/aarch64-none-elf-nm"
OBJCOPY="${BIN}/aarch64-none-elf-objcopy"

# libouncevk-gl.a holds NVK, Zink and EGL in one object, so Vulkan and the
# OpenGL backend share a single driver instance.
SOURCE="${DEVKITPRO}/portlibs/switch2/lib/libouncevk-gl.a"
[[ -f "${SOURCE}" ]] || { echo "OunceVK is missing: ${SOURCE}. Install ounce-devkit." >&2; exit 1; }

mkdir -p "${OUT}"
ARCHIVE="${OUT}/libouncevk_local.a"
SYMBOLS="${OUT}/ouncevk_public_symbols.txt"

# Cemu keeps its Vulkan and GL entry points in global function pointers named
# after the API functions. Both are resolved through vk_icdGetInstanceProcAddr
# and eglGetProcAddress, so the driver's public trampolines can go.
"${NM}" -g --defined-only "${SOURCE}" |
	awk '$NF ~ /^(vk|gl)[A-Z]/ { print $NF }' | LC_ALL=C sort -u > "${SYMBOLS}"
[[ -s "${SYMBOLS}" ]] || { echo "OunceVK has no public entry points to localize" >&2; exit 1; }
# Cemu provides its own copies of these POSIX helpers.
printf '%s\n' writev posix_memalign >> "${SYMBOLS}"
LC_ALL=C sort -u -o "${SYMBOLS}" "${SYMBOLS}"

"${OBJCOPY}" \
	--localize-symbols="${SYMBOLS}" \
	--globalize-symbol=vk_icdGetInstanceProcAddr \
	--globalize-symbol=vk_icdNegotiateLoaderICDInterfaceVersion \
	--globalize-symbol=vk_icdGetPhysicalDeviceProcAddr \
	"${SOURCE}" "${ARCHIVE}.tmp"

for symbol in vk_icdGetInstanceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion \
	eglGetDisplay eglGetProcAddress; do
	if ! "${NM}" -g --defined-only "${ARCHIVE}.tmp" |
		awk -v want="${symbol}" '$NF == want { found = 1 } END { exit !found }'; then
		echo "Localized OunceVK archive does not export ${symbol}" >&2
		rm -f "${ARCHIVE}.tmp"
		exit 1
	fi
done
if "${NM}" -g --defined-only "${ARCHIVE}.tmp" | awk '$NF ~ /^(vk|gl)[A-Z]/ { found = 1 } END { exit !found }'; then
	echo "Localized OunceVK archive still exports a public Vulkan or GL entry point" >&2
	rm -f "${ARCHIVE}.tmp"
	exit 1
fi

mv "${ARCHIVE}.tmp" "${ARCHIVE}"
echo "Localized OunceVK archive: ${ARCHIVE}"
