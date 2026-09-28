# AngryBirdsDesktop

Native Linux desktop loader, runtime shim, and reverse-engineering toolchain for 32-bit x86 Android builds of Angry Birds Classic (Rovio Fusion Engine).

## Overview

AngryBirdsDesktop is an ongoing reverse-engineering and platform reimplementation project designed to execute original 32-bit x86 Android shared libraries (`libAngryBirdsClassic.so`) natively under desktop Linux without full Android virtualization or heavy system emulation layers.

By reconstructing the required Android Bionic C runtime interfaces, standard JNI environments, audio subsystem mixing, and OpenGL ES 2.0 rendering pipelines on top of SDL3, this project acts as a native execution host.

## Current Project Status

- **Status**: Bootable & Rendering / Active Development
- **Boot State**: Fully boots unmodified Android x86 `libAngryBirdsClassic.so` in-memory. The engine completes ELF loading, dynamic symbol resolution, static C++ initialization, JNI setup, GLES2 shader compilation, PVR texture decoding, VFS URI routing, and Lua environment bootstrapping (classes, behaviors, UI hierarchy), actively executing the main application render loop and drawing frames via OpenGL ES 2.0.
- **Binary Integrity**: Zero binary modification or on-disk patching of `libAngryBirdsClassic.so`. All platform adaptations and relocations are handled in-memory.

## Architecture and Components

### 1. In-Memory ELF Loader (`src/loader/`)
- Custom ELF parser and loader mapping unmodified 32-bit x86 Android shared libraries directly from disk into memory.
- Handles `PT_LOAD` segment mapping with exact page permissions (`PROT_READ`, `PROT_WRITE`, `PROT_EXEC`).
- Resolves ELF dynamic relocations (`R_386_RELATIVE`, `R_386_GLOB_DAT`, `R_386_JMP_SLOT`, `R_386_32`).
- Registers `.eh_frame` / `.eh_frame_hdr` sections dynamically with both internal and host unwinders (`__register_frame`), preventing C++ exception aborts across module boundaries.
- Executes `DT_INIT_ARRAY` constructors in order.

### 2. Host Bootstrap & Engine Lifecycle (`src/main.cpp`)
- Creates SDL display window, configures OpenGL ES 2.0 context, manages VSync swap intervals, and captures mouse/keyboard input.
- Drives the Rovio Fusion lifecycle: `JNI_OnLoad` -> `nativeConfig` -> `nativeInit` -> `nativeResize` -> `nativeResume` -> per-frame `nativeUpdate`.
- Features in-memory VFS routing stubs (`VirtualFileSystem::parseUri`) ensuring level bundle assets resolve to `Scheme::VFS`.
- Runtime image format detection hook (`detect_format`) identifying PVRv3 texture streams (`0x03525650` / `0x21505652`).

### 3. Android Bionic & POSIX Shims (`src/android/`)
- **Memory Allocation (`bionic_shims.c`)**: Custom mmap-backed memory allocator (`memalign`, `malloc`, `free`, `realloc`) bypassing glibc 32-bit `sbrk`/top-chunk arena restrictions.
- **Synchronization**: Futex-based lightweight `pthread_mutex` and `pthread_cond` implementations compatible with Bionic expectations.
- **Ctype & Character Classification**: Accurate Android Bionic `_ctype_`, `_tolower_tab_`, and `_toupper_tab_` table shims matching Bionic's `_X`/`_B` bitmasks and pointer-offset expectations for Lua pattern matching.
- **Asset Manager (`asset_manager.c`)**: Emulation of Android `AAssetManager_*` APIs reading directory hierarchies directly from local filesystem paths.
- **Non-Blocking I/O Stubs**: Overrides for `read`/`__read_chk` on standard input descriptors (`STDIN_FILENO`) preventing engine debug consoles from blocking the main execution thread.
- **Logging Subsystem (`log.c`)**: Standardized `__android_log_print` redirection to stdout/stderr.

### 4. JNI Bridge (`src/jni/`)
- Complete emulation of `JNIEnv` and `JavaVM` function tables.
- Synthetic class/method metadata dispatch for Android framework classes, including:
  - `com.rovio.fusion.Globals` (activity handles, cache paths, screen density, metrics)
  - `com.rovio.fusion.SystemFontRenderer` (font metric generation for UI layout trees)
  - `com.rovio.fusion.AudioOutput` (audio mixer context registration)
  - `com.rovio.fusion.DeviceIDCreator`, `com.rovio.rcs.core.Utils`
  - `android.content.Context`, `android.content.res.AssetManager`
  - Java core utilities (`java.util.Locale`, `java.util.UUID`, `java.util.HashMap`, `java.util.List`)

### 5. Audio Subsystem (`src/audio/`)
- Native PCM audio stream consumer utilizing SDL3 audio device callbacks.
- Interfaces with the Rovio engine's native mixer routine (`nativeMixData`).

### 6. Interception & Runtime Hooks (`funchook`)
- Integrated dynamic function hooking via `funchook` and `distorm`:
  - `lua_pcall` / `lua_load` tracing and bytecode error inspection.
  - C++ exception suppression on internal engine Lua failure paths (`sub_3E862`).
  - Drawing state validation (`is_drawing_ready`).

---

## AngryRift Toolchain (`src/angryrift/`)

`angryrift` is a dedicated decryption, decompression, and asset analysis utility tailored for encrypted Lua chunks and resource containers used across Rovio titles:

- **Format Handling**: Automated detection and parsing of Rovio chunk envelopes (Magic headers, payload offsets, checksum headers).
- **AES Decryption**: Standard AES-128-CBC payload decryption utilizing hardcoded and extracted asset keys.
- **LZMA Decompression**: Integrated liblzma stream unpacker for compressed Lua bytecode and JSON asset manifests (`.font.json`, `.sheet.json`, `.compo.json`).
- **Inspection Tools (`luac51f_dump.py`)**: Python-based disassembler and header dumper for custom Lua 5.1/LuaJIT variants.

### Building AngryRift CLI (64-bit standalone)
```bash
g++ -std=c++17 -I src src/angryrift/angryrift.cpp src/angryrift/angryrift_cli.cpp -llzma -o bin/angryrift
```

---

## Building and Running

### Prerequisites (64-bit Linux with Multilib / i686 support)

- GCC and G++ with 32-bit multilib support (`gcc-multilib`, `g++-multilib` or Fedora `glibc-devel.i686`, `libstdc++-devel.i686`)
- CMake 3.16+
- 32-bit development libraries:
  - SDL3 (`libSDL3.so` 32-bit)
  - OpenGL / Mesa (`libGL.so`, `libGLESv2.so`, `libEGL.so` 32-bit)
  - liblzma (optional, for AngryRift 64-bit build)

### Build Instructions

```bash
# Clone the repository
git clone https://github.com/TheAevoraLabs/AngryBirdDesktop.git
cd AngryBirdDesktop

# Configure CMake (32-bit target)
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Build binary and shim libraries
cmake --build build -j$(nproc)
```

The output executable and supporting shims are placed in `bin/angrybirds_desktop`.

---

## License & Disclaimer

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)** - see the [LICENSE](LICENSE) file for details.

Copyright (C) 2026 AevoraLabs / TheAevoraLabs.

This repository contains only independent reverse-engineering tools, shims, and loader code. It does NOT contain copyrighted game assets, binaries, or proprietary libraries from Rovio Entertainment. All trademarks and copyrighted assets belong to their respective owners.
