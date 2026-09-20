# AngryBirdsDesktop

Native Linux desktop loader, runtime shim, and reverse-engineering toolchain for 32-bit x86 Android builds of Angry Birds Classic (Rovio Fusion Engine).

## Overview

AngryBirdsDesktop is an ongoing reverse-engineering and platform reimplementation project designed to execute original 32-bit x86 Android shared libraries (`libAngryBirdsClassic.so`) natively under desktop Linux without full Android virtualization or heavy system emulation layers.

By reconstructing the required Android Bionic C runtime interfaces, standard JNI environments, audio subsystem mixing, and OpenGL ES 2.0 rendering pipelines on top of SDL3, this project acts as a native execution host.

## Current Project Status

- **Status**: Work in Progress / Proof of Concept
- **Boot State**: Experimental; does not boot into a fully playable state out-of-the-box. The loader successfully passes ELF linkage, loads native engine components, establishes the JNI environment, initializes shaders, compiles Lua scripts, initializes audio output, and enters the frame rendering loop, but gameplay transition remains incomplete.

## Architecture and Components

### 1. Host Loader (`src/main.cpp`)
- Handles process bootstrap, 32-bit multilib runtime configuration, and dynamic linker interaction.
- Creates SDL3 display windows, configures OpenGL ES 2.0 contexts, manages VSync swap intervals, and captures input events (mouse, keyboard, window resize).
- Invokes native lifecycle entry points: `JNI_OnLoad`, `nativeConfig`, `nativeInit`, `nativeResume`, `nativeUpdate`, `nativeRender`, and `nativeInput`.

### 2. Android Bionic & POSIX Shims (`src/android/`)
- **Memory Allocation (`bionic_shims.c`)**: Custom mmap-backed memory allocator (`memalign`, `malloc`, `free`, `realloc`) bypassing glibc 32-bit `sbrk`/top-chunk arena restrictions.
- **Synchronization**: Futex-based lightweight `pthread_mutex` and `pthread_cond` implementations compatible with Bionic expectations.
- **Asset Manager (`asset_manager.c`)**: Emulation of Android `AAssetManager_*` APIs reading directory hierarchies directly from local filesystem paths.
- **Non-Blocking I/O Stubs**: Overrides for `read`/`__read_chk` on standard input descriptors (`STDIN_FILENO`) preventing engine debug consoles from blocking the main execution thread.
- **Logging Subsystem (`log.c`)**: Standardized `__android_log_print` redirection to stdout/stderr.

### 3. JNI Bridge (`src/jni/`)
- Complete emulation of `JNIEnv` and `JavaVM` function tables.
- Synthetic class/method metadata dispatch for Android framework classes, including:
  - `com.rovio.fusion.Globals` (activity handles, cache paths, screen density, metrics)
  - `com.rovio.fusion.SystemFontRenderer` (font metric generation for UI layout trees)
  - `com.rovio.fusion.AudioOutput` (audio mixer context registration)
  - `com.rovio.fusion.DeviceIDCreator`, `com.rovio.rcs.core.Utils`
  - `android.content.Context`, `android.content.res.AssetManager`
  - Java core utilities (`java.util.Locale`, `java.util.UUID`, `java.util.HashMap`, `java.util.List`)

### 4. Audio Subsystem (`src/audio/`)
- Native PCM audio stream consumer utilizing SDL3 audio device callbacks.
- Interfaces with the Rovio engine's native mixer routine (`nativeMixData`).

### 5. Interception & Runtime Hooks (`funchook`)
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

This repository contains only independent reverse-engineering tools, shims, and loader code. It does NOT contain copyrighted game assets, binaries, or proprietary libraries from Rovio Entertainment. All trademarks and copyrighted assets belong to their respective owners.
