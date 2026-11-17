if(DEFINED ENV{DEVKITPRO})
	set(DEVKITPRO $ENV{DEVKITPRO})
else()
	set(DEVKITPRO "/opt/devkitpro")
endif()

# Switch 2 builds link libounce and OunceVK from ounce-devkit
# (github.com/ouncebrew/ounce-devkit) in place of libnx and the Mesa SDK.
if(NOT EXISTS "${DEVKITPRO}/libounce/cmake/Switch2.cmake")
	message(FATAL_ERROR "ounce-devkit is not installed in ${DEVKITPRO}: libounce/cmake/Switch2.cmake is missing.")
endif()

include("${DEVKITPRO}/libounce/cmake/Switch2.cmake")

set(PLATFORM_SWITCH ON CACHE BOOL "Target the Nintendo Switch (libnx)" FORCE)
set(PLATFORM_SWITCH2 ON CACHE BOOL "Target the Nintendo Switch 2 (libounce)" FORCE)
set(CEMU_ARCHITECTURE "aarch64" CACHE STRING "" FORCE)

add_compile_definitions(__SWITCH__ __SWITCH2__ NINTENDO_SWITCH NINTENDO_SWITCH2)

add_compile_definitions(BOOST_ASIO_DISABLE_SERIAL_PORT)

add_compile_options(
	-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
	-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=.
	-fno-ident)

set(ENABLE_VCPKG        OFF CACHE BOOL "" FORCE)
set(ENABLE_WXWIDGETS    OFF CACHE BOOL "" FORCE)
# Deko3D targets the Switch 1's Maxwell GPU.
set(CEMU_SWITCH_DEKO3D_RUNTIME OFF CACHE BOOL "" FORCE)
set(ENABLE_DEKO3D       OFF CACHE BOOL "" FORCE)
set(ENABLE_VULKAN       ON  CACHE BOOL "" FORCE)
set(ENABLE_OPENGL       ON  CACHE BOOL "" FORCE)
set(ENABLE_METAL        OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL          OFF CACHE BOOL "" FORCE)
set(ENABLE_CUBEB        OFF CACHE BOOL "" FORCE)
set(ENABLE_DISCORD_RPC  OFF CACHE BOOL "" FORCE)
set(ENABLE_HIDAPI       OFF CACHE BOOL "" FORCE)
set(ENABLE_BLUEZ        OFF CACHE BOOL "" FORCE)
set(ENABLE_WAYLAND      OFF CACHE BOOL "" FORCE)
set(ENABLE_FERAL_GAMEMODE OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBUSB       OFF CACHE BOOL "" FORCE)
set(ALLOW_PORTABLE      OFF CACHE BOOL "" FORCE)
# The bundled profiles were recorded on Switch 1 builds.
set(CEMU_SWITCH_PGO     OFF CACHE STRING "" FORCE)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON CACHE BOOL "" FORCE)
