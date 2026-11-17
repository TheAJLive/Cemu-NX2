# Nintendo Switch 2 build

The Switch 2 build runs as homebrew under [kaiHEN](https://github.com/ouncebrew/kaihen)
and is built with [ounce-devkit](https://github.com/ouncebrew/ounce-devkit):
libounce in place of libnx, OunceVK in place of the Mesa SDK, and libkaihen
for the JIT. The Switch 1 build is unchanged and still uses `build_switch_all.sh`.

## Requirements

- devkitPro with `switch-dev` and the portlibs listed in
  `dist/switch/deps/install_devkitpro_packages.sh` (`switch-mesa` and
  `switch-libdrm_nouveau` are not needed for Switch 2).
- ounce-devkit 20261115b or newer, installed into the same devkitPro:

  ```sh
  git clone https://github.com/ouncebrew/ounce-devkit
  cd ounce-devkit && sh install.sh
  ```

- A Linux host (or WSL) with git, cmake, ninja, g++, curl, bison and flex.

The private Mesa SDK used by the Switch 1 build is not needed.

## Build

```sh
git submodule update --init --depth 1 dependencies/ZArchive \
    dependencies/Vulkan-Headers dependencies/imgui dependencies/xbyak_aarch64
BUILD_JOBS=8 ./build_switch2_all.sh
```

This builds the emulator core (`bin/switch2/cemu_core.nro`) and embeds it in
the SDL launcher, `switch_launcher/switch2/cemu.nro`. To build only the core,
run `dist/switch2/build.sh`.

## Install

1. Set up kaiHEN on the console as its release describes, so that holding R
   while starting a game opens hbmenu.
2. Copy `switch_launcher/switch2/cemu.nro` anywhere under `sdmc:/switch/`,
   for example `sdmc:/switch/cemu.nro`. Cemu returns to it after a game.
3. Start a game holding R and open Cemu from hbmenu once, then exit it. The
   first launch creates the Cemu folders under `sdmc:/switch/cemu/` (`games`,
   `graphicPacks`, `mlc01` and the rest). An existing Switch 1 setup on the SD
   card can be reused as is, so this step can be skipped.
4. Put your games in `sdmc:/switch/cemu/games/` (`.wua`, `.wud`, `.wux`,
   `.wuhb` and `.rpx` all work). Disc keys go in `sdmc:/switch/cemu/keys.txt`,
   one per line, as on desktop Cemu. WUA files don't need them.
5. Start a game holding R, then open Cemu from hbmenu.

## What differs from Switch 1

- **Renderers:** Vulkan on OunceVK, and OpenGL through OunceVK's Zink. Deko3D
  targets the Switch 1 GPU and is not built.
- **JIT:** kaiHEN does not give games the code memory syscalls. Recompiled code
  is written to a staging buffer and published to the executable mapping
  through kaiHEN's RO service (`kaihenJitPublishRanges`).
- **Guest memory:** the 4GB Wii U address window is carved out of the heap
  that hbloader hands to the NRO, so the console must leave at least about
  5.5GB of heap to homebrew. The window can be imported by the GPU directly.
- **Cache maintenance** uses EL0 `dc cvac`/`dc civac` instead of the cache
  syscalls.
- **CPU:** the three Espresso cores stay on cores 0-2. The GPU thread gets the
  first core the game process is granted beyond those, the audio mixer the
  last, and the recompiler worker and compile pools use the rest. Pipeline
  compiling scales with the helper cores (up to 4 threads, 8 while the cache
  loads), and the JIT uses LSE atomics and can grow to 256MB of code.
- **Memory:** with a 9GB game heap, Cemu's own heap gets about 4.3GB after the
  guest window, and the shader module cache is 256MB instead of 64MB.
- **Not available yet:** USB drives (opening `usb:hs` breaks the next
  homebrew under kaiHEN) and SaltyNX reporting. HOME Menu shortcuts are left
  out on purpose.
- On exit, threads that were never joined are paused and their stacks are
  unmapped, so hbmenu gets its memory back when Cemu returns.

## Launcher updates

The launcher checks the latest release of `TheAJLive/Cemu-NX2` on GitHub.
The Switch 2 launcher only takes a release asset named `cemu-switch2.nro`
(any `.nro` with `switch2` in its name), and a Switch 1 launcher skips those, so
both builds can be attached to the same release. Tag releases with the same
version passed as `RELEASE_VERSION`; the updater only offers a newer tag.
