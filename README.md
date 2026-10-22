# Nodosumi

Nodosumi is a SEGA Dreamcast emulator core written in C, with a focus on
low-latency, full-speed emulation on small ARM handhelds as well as desktop
Linux, macOS, and Windows. It ships as a standalone libretro core and as a
desktop application.

## Features

- SH-4 and ARM7 interpreters, plus x86-64 and AArch64 JIT backends
- PowerVR2 (PVR) hardware renderer on OpenGL ES 3.0+
- AICA audio, GD-ROM, Maple input, and Dreamcast BIOS/Flash handling
- Byte-exact save states, including cross-session round-trips
- Multi-disc support through `.m3u` playlists
- Libretro core with disk control and core options

## Requirements

- A Dreamcast BIOS (`dc/boot.bin`) is required. No BIOS, ROM, or game data is
  included with Nodosumi.
- OpenGL ES 3.0 or newer for the hardware renderer.
- CMake 3.5+ and a C11/C++11 toolchain.

## Building the libretro core

```bash
cmake -S . -B build -G Ninja \
  -DBUILD_LIBRETRO=ON \
  -DBUILD_TOOLS=OFF \
  -DBUILD_TESTS=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The resulting core is `build/libnodosumi.so` (`libnodosumi.dylib` on macOS).
Copy it into RetroArch's `cores/` directory next to a matching `.info` file.

For an AArch64 build with the JIT enabled, add:

```bash
-DA64_JIT=ON -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux.cmake
```

## Building the desktop application

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The executable is `build/nodosumi`.

## Usage

- Place the Dreamcast BIOS at `<system_directory>/dc/boot.bin`.
- Load `.cdi`, `.chd`, `.gdi`, `.cue`, or `.m3u` disc images. Disc images must
  be passed as full paths; they are not extracted from archives.
- Core options control region, language, broadcast standard, aspect ratio, and
  statistics logging.
- Set `NODOSUMI_STATS=1` to log per-frame timing, and `NODOSUMI_PERF=1` to
  write a JIT symbol map for `perf`.

## Status

Nodosumi 1.0 is the first public release. The emulation core is stable enough
to run a broad library of Dreamcast software; compatibility and performance
work continues.

## License

Nodosumi is free software licensed under the GNU General Public License,
version 3 or later. See [LICENSE.txt](LICENSE.txt).

"SEGA" and "Dreamcast" are trademarks of SEGA Corporation. Nodosumi is not
affiliated with or endorsed by SEGA.
