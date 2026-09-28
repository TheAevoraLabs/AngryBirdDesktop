#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <filesystem>
#include <chrono>
#include <thread>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengles2.h>

#include "loader/so_util.h"
#include "jni/jni.h"
#include "jni/jni_fake.h"
#include "fusion/fusion.h"
#include "audio/audio.h"
#include "input/input.h"
#include "render/present.h"
#include "crash/crash.h"
#include "patches/powerup_patch.h"
#include "patches/store_patch.h"
#include "patches/iap_patch.h"
#include "common/game_config.h"
#include "common/util.h"
#include "android/asset_manager.h"
#include "android/log.h"

namespace fs = std::filesystem;

// Engine module instance
static so_module g_game_mod;

// Engine entry point prototypes
typedef jint (*JNI_OnLoad_t)(JavaVM vm, void* reserved);
typedef void (*nativeConfig_t)(JNIEnv env, jobject thiz, jstring path);
typedef void (*nativeInit_t)(JNIEnv env, jobject thiz, jint width, jint height);
typedef void (*nativeDeinit_t)(JNIEnv env, jobject thiz);
typedef void (*nativePause_t)(JNIEnv env, jobject thiz);
typedef void (*nativeResume_t)(JNIEnv env, jobject thiz);
typedef void (*nativeResize_t)(JNIEnv env, jobject thiz, jint width, jint height);
/* nativeUpdate()Z -- the Java layer treats a false return as "the engine is
 * finished", tears the GL surface down and calls nativeDeinit. Ignoring the
 * return value means we keep driving an engine that has already begun shutting
 * its own threads down, which is exactly how a clean window close turns into a
 * crash. */
typedef jboolean (*nativeUpdate_t)(JNIEnv env, jobject thiz);
typedef jboolean (*nativeRender_t)(JNIEnv env, jobject thiz);
/* Argument order matters on x86-32: Fusion's Java layer declares
 *   nativeInput(int action, float x, float y, int pointerId)
 *   nativeKeyInput(int keyCode, int action, int unicodeChar, int deviceId)
 * (the AArch64 Switch port could be sloppy about this and still work, because
 * ints and floats travel in different register banks there -- cdecl has no such
 * luxury, so the order below is taken straight from the engine's prologues). */
typedef void (*nativeInput_t)(JNIEnv env, jobject thiz, jint action, jfloat x, jfloat y, jint pointerId);
typedef void (*nativeKeyInput_t)(JNIEnv env, jobject thiz, jint keyCode, jint action, jint unicodeChar, jint deviceId);
typedef void (*nativeInputAxis_t)(JNIEnv env, jobject thiz, jint axis, jfloat value, jint deviceId);

static JNI_OnLoad_t g_JNI_OnLoad = nullptr;
static nativeConfig_t g_nativeConfig = nullptr;
static nativeInit_t g_nativeInit = nullptr;
static nativeDeinit_t g_nativeDeinit = nullptr;
static nativePause_t g_nativePause = nullptr;
static nativeResume_t g_nativeResume = nullptr;
static nativeResize_t g_nativeResize = nullptr;
static nativeUpdate_t g_nativeUpdate = nullptr;
static nativeRender_t g_nativeRender = nullptr;
static nativeInput_t g_nativeInput = nullptr;
static nativeKeyInput_t g_nativeKeyInput = nullptr;
static nativeInputAxis_t g_nativeInputAxis = nullptr;
static void* g_nativeMixData_addr = nullptr;

// External Bionic symbols from bionic_shims.c
extern "C" {
    extern const short* _tolower_tab_;
    extern const short* _toupper_tab_;
    extern const char*  _ctype_;
    extern FILE __sF[3];
    extern void* memalign(size_t alignment, size_t size);
    extern size_t malloc_usable_size(void* ptr);
    extern ssize_t __read_chk(int fd, void *buf, size_t count, size_t buflen);

    // Bionic's LP32 `struct sigaction` is 16 bytes (its sigset_t is one word),
    // glibc's is 140. The engine allocates the Bionic-sized struct on its stack
    // and hands the address to sigaction(), so calling glibc's directly walks
    // ~124 bytes past the end of that stack slot. These wrappers convert the
    // layouts; they are deliberately not named `sigaction`/`sigprocmask` so the
    // host libraries keep calling the real ones.
    struct bionic_sigaction;
    extern int rovio_sigaction_compat(int signum, const struct bionic_sigaction* act, struct bionic_sigaction* oldact);
    extern int rovio_sigprocmask_compat(int how, const unsigned long* set, unsigned long* oldset);
}

static const so_default_dynlib g_bionic_dynlib[] = {
    { "__android_log_print", (uintptr_t)__android_log_print },
    { "__android_log_write", (uintptr_t)__android_log_write },
    { "AAssetManager_fromJava", (uintptr_t)AAssetManager_fromJava },
    { "AAssetManager_open", (uintptr_t)AAssetManager_open },
    { "AAsset_close", (uintptr_t)AAsset_close },
    { "AAsset_read", (uintptr_t)AAsset_read },
    { "AAsset_seek", (uintptr_t)AAsset_seek },
    { "AAsset_seek64", (uintptr_t)AAsset_seek64 },
    { "AAsset_getLength", (uintptr_t)AAsset_getLength },
    { "AAsset_getLength64", (uintptr_t)AAsset_getLength64 },
    { "AAsset_getRemainingLength", (uintptr_t)AAsset_getRemainingLength },
    { "AAsset_getRemainingLength64", (uintptr_t)AAsset_getRemainingLength64 },
    { "AAsset_getBuffer", (uintptr_t)AAsset_getBuffer },
    { "AAsset_isAllocated", (uintptr_t)AAsset_isAllocated },
    { "AAsset_openFileDescriptor", (uintptr_t)AAsset_openFileDescriptor },
    { "AAsset_openFileDescriptor64", (uintptr_t)AAsset_openFileDescriptor64 },
    { "AAssetManager_openDir", (uintptr_t)AAssetManager_openDir },
    { "AAssetDir_getNextFileName", (uintptr_t)AAssetDir_getNextFileName },
    { "AAssetDir_rewind", (uintptr_t)AAssetDir_rewind },
    { "AAssetDir_close", (uintptr_t)AAssetDir_close },
    { "_ctype_", (uintptr_t)&_ctype_ },
    { "_tolower_tab_", (uintptr_t)&_tolower_tab_ },
    { "_toupper_tab_", (uintptr_t)&_toupper_tab_ },
    { "__sF", (uintptr_t)&__sF },
    { "memalign", (uintptr_t)memalign },
    { "malloc_usable_size", (uintptr_t)malloc_usable_size },
    { "__read_chk", (uintptr_t)__read_chk },
    { "sigaction", (uintptr_t)rovio_sigaction_compat },
    { "sigprocmask", (uintptr_t)rovio_sigprocmask_compat },
};

static bool resolve_engine_symbols(so_module* mod) {
    g_JNI_OnLoad = (JNI_OnLoad_t)so_symbol(mod, "JNI_OnLoad");
    g_nativeConfig = (nativeConfig_t)so_symbol(mod, "Java_com_rovio_fusion_NativeApplication_nativeConfig");
    g_nativeInit = (nativeInit_t)so_symbol(mod, "Java_com_rovio_fusion_NativeApplication_nativeInit");
    g_nativeDeinit = (nativeDeinit_t)so_symbol(mod, "Java_com_rovio_fusion_NativeApplication_nativeDeinit");
    g_nativePause = (nativePause_t)so_symbol(mod, "Java_com_rovio_fusion_NativeApplication_nativePause");
    g_nativeResume = (nativeResume_t)so_symbol(mod, "Java_com_rovio_fusion_NativeApplication_nativeResume");
    g_nativeResize = (nativeResize_t)so_symbol(mod, "Java_com_rovio_fusion_NativeApplication_nativeResize");
    g_nativeUpdate = (nativeUpdate_t)so_symbol(mod, "Java_com_rovio_fusion_NativeApplication_nativeUpdate");
    g_nativeRender = (nativeRender_t)so_symbol(mod, "Java_com_rovio_fusion_NativeApplication_nativeRender");
    g_nativeInput = (nativeInput_t)so_symbol(mod, "Java_com_rovio_fusion_MyInputHandler_nativeInput");
    g_nativeKeyInput = (nativeKeyInput_t)so_symbol(mod, "Java_com_rovio_fusion_MyInputHandler_nativeKeyInput");
    g_nativeInputAxis = (nativeInputAxis_t)so_symbol(mod, "Java_com_rovio_fusion_MyInputHandler_nativeInputAxis");
    // The exported mixer lives on AudioOutput, not on an "Audio" class: see
    // `nm -D libAngryBirdsClassic.so | grep nativeMixData`.
    g_nativeMixData_addr = (void*)so_symbol(mod, "Java_com_rovio_fusion_AudioOutput_nativeMixData");
    if (!g_nativeMixData_addr)
        g_nativeMixData_addr = (void*)so_symbol(mod, "Java_com_rovio_fusion_Audio_nativeMixData");

    printf("[Engine] Resolved native entry points:\n");
    printf("  JNI_OnLoad:     %p\n", (void*)g_JNI_OnLoad);
    printf("  nativeConfig:   %p\n", (void*)g_nativeConfig);
    printf("  nativeInit:     %p\n", (void*)g_nativeInit);
    printf("  nativeResize:   %p\n", (void*)g_nativeResize);
    printf("  nativeUpdate:   %p\n", (void*)g_nativeUpdate);
    printf("  nativeInput:    %p\n", (void*)g_nativeInput);
    printf("  nativeKeyInput: %p\n", (void*)g_nativeKeyInput);
    printf("  nativeMixData:  %p\n", g_nativeMixData_addr);
    fflush(stdout);

    if (!g_nativeInit || !g_nativeUpdate) {
        printf("[Error] Missing core symbols in game SO!\n");
        return false;
    }
    if (!g_nativeInput)
        printf("[Warning] nativeInput missing -- pointer input will be dead.\n");
    if (!g_nativeMixData_addr)
        printf("[Warning] nativeMixData missing -- the mixer cannot be registered.\n");
    return true;
}

#include <funchook.h>

// Lua hooks and error handling
typedef int (*lua_pcall_t)(void *L, int nargs, int nresults, int errfunc);
typedef const char* (*lua_tolstring_t)(void *L, int idx, size_t *len);
typedef int (*is_drawing_ready_t)(void* obj);

static lua_pcall_t g_orig_lua_pcall = nullptr;
static lua_tolstring_t g_lua_tolstring = nullptr;
static is_drawing_ready_t g_orig_is_drawing_ready = nullptr;

static int my_lua_pcall(void *L, int nargs, int nresults, int errfunc) {
    int res = g_orig_lua_pcall(L, nargs, nresults, errfunc);
    if (res != 0) {
        const char *err = g_lua_tolstring ? g_lua_tolstring(L, -1, nullptr) : "(no tolstring)";
        printf("[Lua] Error in lua_pcall: %d -> %s\n", res, err ? err : "(null)");

        // Walk Lua CallInfo stack
        if (L) {
            uint8_t *state = (uint8_t*)L;
            uint8_t *ci = *(uint8_t**)(state + 0x14); // L->ci
            uint8_t *base_ci = *(uint8_t**)(state + 0x28); // L->base_ci (approx)
            printf("[Lua Traceback]:\n");
            int frame = 0;
            while (ci && frame < 30) {
                uint8_t *func_tv = *(uint8_t**)(ci + 0x4); // ci->func
                if (!func_tv) break;
                uint32_t tt = *(uint32_t*)(func_tv + 4);
                if (tt == 6) { // LUA_TFUNCTION
                    uint8_t *cl = *(uint8_t**)(func_tv + 0);
                    if (cl) {
                        uint8_t isC = cl[7];
                        if (!isC) {
                            uint8_t *proto = *(uint8_t**)(cl + 8);
                            if (proto) {
                                uint8_t *src_str = *(uint8_t**)(proto + 8);
                                const char *src = src_str ? (const char*)(src_str + 0x10) : "(unknown)";
                                int linedefined = *(int*)(proto + 0xc);
                                printf("  #%d [Lua] %s (line defined: %d)\n", frame, src, linedefined);
                            }
                        } else {
                            printf("  #%d [C/C++ func %p]\n", frame, *(void**)(cl + 8));
                        }
                    }
                }
                // Previous CallInfo in array is (ci - 0x18)
                ci = ci - 0x18;
                frame++;
            }
        }
        fflush(stdout);
    }
    return res;
}

typedef int (*lua_load_t)(void *L, void *reader, void *data, const char *chunkname);
static lua_load_t g_orig_lua_load = nullptr;

static int my_lua_load(void *L, void *reader, void *data, const char *chunkname) {
    printf("[Lua] Loading chunk: %s\n", chunkname ? chunkname : "(null)");
    fflush(stdout);
    return g_orig_lua_load(L, reader, data, chunkname);
}

typedef void (*luaG_runerror_t)(void *L, const char *fmt, ...);
static luaG_runerror_t g_orig_luaG_runerror = nullptr;

static void my_luaG_runerror(void *L, const char *fmt, ...) {
    char buf[1024] = {0};
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);

    const char *source = "(unknown)";
    int line = 0;
    if (L) {
        uint8_t *state = (uint8_t*)L;
        uint8_t *ci = *(uint8_t**)(state + 0x14); // L->ci
        if (ci) {
            uint8_t *func_tv = *(uint8_t**)(ci + 0x4); // ci->func
            if (func_tv && *(uint32_t*)(func_tv + 4) == 6) { // LUA_TFUNCTION
                uint8_t *cl = *(uint8_t**)(func_tv + 0);
                if (cl && cl[6] == 0) { // isC == 0
                    uint8_t *proto = *(uint8_t**)(cl + 0x10);
                    if (proto) {
                        uint8_t *src_obj = *(uint8_t**)(proto + 0x20); // Proto->source
                        if (src_obj) source = (const char*)(src_obj + 0x10);
                        uint32_t *code = *(uint32_t**)(proto + 0xc);
                        int *lineinfo = *(int**)(proto + 0x14);
                        uint32_t *savedpc = *(uint32_t**)(state + 0x18);
                        if (code && lineinfo && savedpc && savedpc > code) {
                            int pc = (int)(savedpc - code) - 1;
                            line = lineinfo[pc];
                        }
                    }
                }
            }
        }
    }

    printf("[Lua Panic] in %s:%d: %s\n", source, line, buf);
    fflush(stdout);
    g_orig_luaG_runerror(L, "%s", buf);
}

static int my_is_drawing_ready(void* obj) {
    if (!obj) return 0;
    return 1;
}

static void setup_vfs_bundle_routing(so_module* mod) {
    if (!mod || !mod->base) return;

    uint8_t *stub = (uint8_t *)mmap(
        NULL, 4096,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0
    );
    if (stub == MAP_FAILED) return;

    uint8_t *p = stub;
    // 8b bd d8 fd ff ff        mov    -0x228(%ebp),%edi
    *p++ = 0x8b; *p++ = 0xbd; *p++ = 0xd8; *p++ = 0xfd; *p++ = 0xff; *p++ = 0xff;
    // c7 07 05 00 00 00        movl   $0x5,(%edi)
    *p++ = 0xc7; *p++ = 0x07; *p++ = 0x05; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    // 8b 85 dc fd ff ff        mov    -0x224(%ebp),%eax
    *p++ = 0x8b; *p++ = 0x85; *p++ = 0xdc; *p++ = 0xfd; *p++ = 0xff; *p++ = 0xff;
    // 89 44 24 04              mov    %eax,0x4(%esp)
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x04;
    // 8d 47 04                 lea    0x4(%edi),%eax
    *p++ = 0x8d; *p++ = 0x47; *p++ = 0x04;
    // 89 04 24                 mov    %eax,(%esp)
    *p++ = 0x89; *p++ = 0x04; *p++ = 0x24;

    // e8 xx xx xx xx           call   (mod->base + 0x9ec020)
    *p++ = 0xe8;
    int32_t rel_call = (int32_t)((mod->base + 0x9ec020) - (p + 4));
    memcpy(p, &rel_call, 4);
    p += 4;

    // e9 xx xx xx xx           jmp    (mod->base + 0x756d0f)
    *p++ = 0xe9;
    int32_t rel_jmp = (int32_t)((mod->base + 0x756d0f) - (p + 4));
    memcpy(p, &rel_jmp, 4);
    p += 4;

    auto hook_jmp6 = [](uint8_t *src, uint8_t *dst) {
        src[0] = 0xe9;
        int32_t rel = (int32_t)(dst - (src + 5));
        memcpy(src + 1, &rel, 4);
        src[5] = 0x90;
    };

    hook_jmp6(mod->base + 0x756e97, stub);
    hook_jmp6(mod->base + 0x756f1c, stub);
    hook_jmp6(mod->base + 0x756f5a, stub);

    // Also patch 0x756f8b (7 bytes) -> jmp stub
    uint8_t *p_throw = mod->base + 0x756f8b;
    p_throw[0] = 0xe9;
    int32_t rel_throw = (int32_t)(stub - (p_throw + 5));
    memcpy(p_throw + 1, &rel_throw, 4);
    p_throw[5] = 0x90;
    p_throw[6] = 0x90;

    printf("[Hook] In-memory VFS bundle routing installed (stub at %p)\n", stub);
}

typedef void (*image_reader_create_t)(void *out_reader, void **stream, int format);
static image_reader_create_t g_orig_image_reader_create = nullptr;

static void my_image_reader_create(void *out_reader, void **stream, int format) {
    printf("[ImageReader] create reader: out=%p, stream=%p, format=%d\n", out_reader, stream ? *stream : nullptr, format);
    fflush(stdout);
    g_orig_image_reader_create(out_reader, stream, format);
}

typedef int (*detect_format_t)(void *stream);
static detect_format_t g_orig_detect_format = nullptr;

typedef int (*stream_read_t)(void *stream, void *buf, size_t size);
typedef int (*stream_tell_t)(void *stream);
typedef void (*stream_seek_t)(void *stream, int pos, int origin, int u);

static int my_detect_format(void *stream) {
    int fmt = g_orig_detect_format(stream);
    if (fmt == 0 && stream) {
        void **vtable = *(void***)stream;
        if (vtable && vtable[2] && vtable[4] && vtable[5]) {
            stream_tell_t tell_fn = (stream_tell_t)vtable[5];
            stream_seek_t seek_fn = (stream_seek_t)vtable[4];
            stream_read_t read_fn = (stream_read_t)vtable[2];
            int old_pos = tell_fn(stream);
            seek_fn(stream, 0, 0, 0);
            uint32_t magic = 0;
            read_fn(stream, &magic, 4);
            seek_fn(stream, old_pos, 0, 0);

            if (magic == 0x03525650 || magic == 0x21505652) {
                fmt = 11; // PVR
            } else if (magic == 0x474e5089) {
                fmt = 6;  // PNG
            } else if ((magic & 0xffff) == 0xd8ff) {
                fmt = 3;  // JPG
            } else if ((magic & 0xffff) == 0x4d42) {
                fmt = 1;  // BMP
            }
            printf("[ImageReader] stream %p: auto-detected magic 0x%08x -> format %d\n", stream, magic, fmt);
        }
    }
    printf("[ImageReader] detect_format -> format=%d\n", fmt);
    fflush(stdout);
    return fmt;
}

static void install_runtime_hooks(so_module* mod) {
    if (!mod || !mod->base) return;

    g_orig_lua_pcall = (lua_pcall_t)(mod->base + 0x8d66f0);
    g_lua_tolstring = (lua_tolstring_t)(mod->base + 0x8d49f0);
    g_orig_lua_load = (lua_load_t)(mod->base + 0x8d68d0);
    g_orig_luaG_runerror = (luaG_runerror_t)(mod->base + 0x8c7160);
    g_orig_is_drawing_ready = (is_drawing_ready_t)(mod->base + 0x2176a0);
    g_orig_image_reader_create = (image_reader_create_t)(mod->base + 0x70fa00);
    g_orig_detect_format = (detect_format_t)(mod->base + 0x74b160);

    funchook_t *funchook = funchook_create();
    if (funchook) {
        funchook_prepare(funchook, (void**)&g_orig_lua_pcall, (void*)my_lua_pcall);
        funchook_prepare(funchook, (void**)&g_orig_lua_load, (void*)my_lua_load);
        funchook_prepare(funchook, (void**)&g_orig_luaG_runerror, (void*)my_luaG_runerror);
        funchook_prepare(funchook, (void**)&g_orig_is_drawing_ready, (void*)my_is_drawing_ready);
        funchook_prepare(funchook, (void**)&g_orig_image_reader_create, (void*)my_image_reader_create);
        funchook_prepare(funchook, (void**)&g_orig_detect_format, (void*)my_detect_format);
        int rv = funchook_install(funchook, 0);
        printf("[Hook] In-memory funchook installed (result: %d)\n", rv);
    }

    setup_vfs_bundle_routing(mod);
}

int main(int argc, char* argv[]) {
    // 0. Crash-dialog mode: our own signal handler re-executes this binary with
    // --crash-report <file> so the window is drawn by a healthy process.
    {
        char report[600] = {0};
        if (crash_gui_mode(argc, argv, report, sizeof(report))) {
            if (!report[0]) {
                fprintf(stderr, "usage: %s --crash-report <file>\n", argv[0]);
                return 2;
            }
            return crash_show_dialog(report);
        }
    }

    printf("=========================================\n");
    printf("   Angry Birds Desktop (In-Memory x86)   \n");
    printf("=========================================\n");

    // 1. Filesystem & directories
    fs::create_directories("save");
    fs::create_directories("save/cache");
    config_load(CONFIG_NAME);
    screen_width = config.width;
    screen_height = config.height;

    char res_desc[32];
    if (config.width == 0) snprintf(res_desc, sizeof(res_desc), "desktop");
    else snprintf(res_desc, sizeof(res_desc), "%dx%d", config.width, config.height);

    crash_init(argv && argv[0] ? argv[0] : nullptr, DATA_DIR);
    printf("[Config] %s fullscreen=%d vsync=%d scaler=%s renderScale=%d%% "
           "powerups=%s money=%s language=%s\n",
           res_desc, config.fullscreen, config.vsync,
           config.scaler, config.render_scale, config.powerups, config.money,
           config.language);

    // Save-side extras have to land before the engine reads its save (which it
    // does during nativeInit), so do them here, before SDL even starts.
    powerup_patch_init();
    store_patch_init();
    printf("iap: %s\n", config.iap ? "on" : "off");

    if (getenv("AB_CRASH_TEST")) crash_test_trigger();

    if (fs::exists("assets")) {
        AAssetManager_setBasePath("assets");
    } else if (fs::exists("../assets")) {
        AAssetManager_setBasePath("../assets");
    } else {
        printf("[Warning] 'assets' directory not found in current path!\n");
    }

    // 2. Initialize SDL3
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        printf("[SDL3] Failed to initialize SDL: %s\n", SDL_GetError());
        return 1;
    }

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        printf("[SDL3] Gamepad subsystem unavailable (%s)\n", SDL_GetError());
    }

    // `width 0` / `height 0` (the default) means "the monitor's resolution".
    // Fusion lays its UI out from the size we hand it, so booting at the
    // panel's native size is the sharpest option without a scaler.
    if (screen_width <= 0 || screen_height <= 0) {
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (dm && dm->w > 0 && dm->h > 0) {
            if (screen_width <= 0) screen_width = dm->w;
            if (screen_height <= 0) screen_height = dm->h;
        } else {
            if (screen_width <= 0) screen_width = 1280;
            if (screen_height <= 0) screen_height = 720;
        }
        printf("[SDL3] desktop resolution %dx%d\n", screen_width, screen_height);
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow(
        "Angry Birds Classic",
        screen_width,
        screen_height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        printf("[SDL3] Failed to create window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // SDL3's plain fullscreen is desktop/borderless, which is what a game like
    // this wants (no mode switch, no flicker). F11 / Alt+Enter toggles it.
    bool fullscreen = config.fullscreen != 0;
    if (fullscreen) SDL_SetWindowFullscreen(window, true);

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        printf("[SDL3] Failed to create OpenGL context: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!SDL_GL_MakeCurrent(window, gl_context)) {
        printf("[SDL3] Failed to make OpenGL context current: %s\n", SDL_GetError());
        SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_SetSwapInterval(config.vsync ? 1 : 0);

    // The engine and the window can now differ: with a scaler other than
    // "native" the engine renders at renderScale% of the window and we upscale.
    PresentConfig pcfg;
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.scaler = present_scaler_from_name(config.scaler);
    pcfg.render_scale = config.render_scale;
    pcfg.window_w = screen_width;
    pcfg.window_h = screen_height;
    present_init(&pcfg);

    int win_w = screen_width, win_h = screen_height;
    screen_width = present_render_w();
    screen_height = present_render_h();

    // 3. Input front-end (mouse + multi-touch + keyboard -> Fusion nativeInput).
    // Mode is overridable for testing: AB_INPUT=mouse|touch|auto, AB_HOVER=0|1.
    AbInputConfig icfg;
    memset(&icfg, 0, sizeof(icfg));
    icfg.render_w = screen_width;
    icfg.render_h = screen_height;
    icfg.window_w = win_w;
    icfg.window_h = win_h;
    icfg.mode = AB_INPUT_AUTO;
    icfg.mouse_hover = 1;
    icfg.mouse_pointer_id = 0;
    icfg.keyboard = 1;
    icfg.max_pointers = 8;
    if (const char* m = getenv("AB_INPUT")) {
        if (!strcmp(m, "mouse")) icfg.mode = AB_INPUT_MOUSE;
        else if (!strcmp(m, "touch")) icfg.mode = AB_INPUT_TOUCH;
    }
    if (const char* h = getenv("AB_HOVER")) icfg.mouse_hover = (h[0] != '0');
    ab_input_init(&icfg);

    // 4. Initialize Fake JNI Environment
    jni_init();
    void* thiz = jni_make_thiz();

    // 5. In-Memory ELF Loading (Unmodified libAngryBirdsClassic.so)
    const char* lib_paths[] = {
        "bin/libAngryBirdsClassic.so",
        "angry-birds-classic-8-0-3/lib/x86/libAngryBirdsClassic.so",
        "./libAngryBirdsClassic.so",
        "../bin/libAngryBirdsClassic.so"
    };

    const char* selected_path = nullptr;
    for (const char* p : lib_paths) {
        if (fs::exists(p)) {
            selected_path = p;
            break;
        }
    }

    if (!selected_path) {
        printf("[Error] Could not find libAngryBirdsClassic.so!\n");
        SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    printf("[Loader] Loading in-memory ELF from %s...\n", selected_path);
    if (so_load(&g_game_mod, selected_path) != 0) {
        printf("[Error] Failed to parse and map ELF module!\n");
        SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    crash_set_module("libAngryBirdsClassic.so", (uintptr_t)g_game_mod.base, g_game_mod.size);

    printf("[Loader] Applying internal relocations...\n");
    so_relocate(&g_game_mod);

    // The shop talks to Google Play through Java we do not have; this stands in
    // for it so a purchase tap actually completes. Needs the relocated module
    // (it calls the engine's own paymentFinished/restoreDone exports).
    iap_patch_init(&g_game_mod, config.iap);

    printf("[Loader] Resolving dynamic imports...\n");
    so_resolve(&g_game_mod, g_bionic_dynlib, sizeof(g_bionic_dynlib) / sizeof(g_bionic_dynlib[0]));

    printf("[Loader] Running static initializers (.init_array)...\n");
    so_initialize(&g_game_mod);

    if (!resolve_engine_symbols(&g_game_mod)) {
        printf("[Error] Failed to resolve required engine symbols!\n");
        so_free(&g_game_mod);
        SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    install_runtime_hooks(&g_game_mod);

    // 6. Register Audio Mixer (AB_AUDIO=0 keeps it silent, e.g. for bisecting
    // a crash that only shows up once the engine's mixer is being driven).
    if (g_nativeMixData_addr) {
        const char* audio_env = getenv("AB_AUDIO");
        if (audio_env && audio_env[0] == '0') {
            printf("[Audio] disabled by AB_AUDIO=0\n");
        } else {
            audio_set_mixer(g_nativeMixData_addr, thiz);
        }
    }

    // 7. JNI OnLoad & Native Configuration
    if (g_JNI_OnLoad) {
        printf("[Game] Calling JNI_OnLoad...\n");
        g_JNI_OnLoad(fake_vm, nullptr);
    }

    if (g_nativeConfig) {
        printf("[Game] Calling nativeConfig...\n");
        jstring cfg_path = jni_make_string("./save");
        g_nativeConfig(fake_env, thiz, cfg_path);
    }

    printf("[Game] Initializing engine with resolution %dx%d (window %dx%d)...\n",
           screen_width, screen_height, win_w, win_h);
    if (g_nativeInit) {
        g_nativeInit(fake_env, thiz, screen_width, screen_height);
    }
    printf("[Game] nativeInit completed successfully!\n");

    if (g_nativeResize) {
        printf("[Game] Resizing engine viewport to %dx%d...\n", screen_width, screen_height);
        g_nativeResize(fake_env, thiz, screen_width, screen_height);
    }

    if (g_nativeResume) {
        printf("[Game] Resuming native application...\n");
        g_nativeResume(fake_env, thiz);
        printf("[Game] nativeResume completed successfully!\n");
    }

    // 8. Main Application Loop
    bool running = true;
    SDL_Event event;
    uint64_t frame_count = 0;

    /* Self-test hooks, so a headless-ish harness can exercise things that would
     * otherwise need a human at the mouse:
     *   AB_AUTOCLOSE=<frames>       quit through the real window-close path
     *   AB_SCREENSHOT=<file.bmp>    save one frame right before it is swapped
     *   AB_SCREENSHOT_FRAME=<n>     which frame to save (default 600) */
    const char *shot_path = getenv("AB_SCREENSHOT");
    long shot_frame = 600;
    if (const char *s = getenv("AB_SCREENSHOT_FRAME")) {
        long v = atol(s);
        if (v > 0) shot_frame = v;
    }
    long autoclose = 0;
    if (const char *s = getenv("AB_AUTOCLOSE")) {
        autoclose = atol(s);
        if (autoclose < 1) autoclose = 1;
    }
    const char *fake_purchase = getenv("AB_FAKE_PURCHASE");
    long fake_purchase_frame = 240;
    if (const char *s = getenv("AB_FAKE_PURCHASE_FRAME")) {
        long v = atol(s);
        if (v > 0) fake_purchase_frame = v;
    }

    printf("[Game] Entering main render loop...\n");
    fflush(stdout);

    while (running && !jni_quit_requested) {
        uint64_t start_time = SDL_GetTicks();

        // Poll Events. App-level keys are handled here; everything else is
        // handed to the input module, which translates it into engine events.
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                printf("[Game] Received SDL_EVENT_QUIT\n");
                running = false;
                continue;
            }

            if (event.type == SDL_EVENT_KEY_DOWN &&
                (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q)) {
                printf("[Game] ESC/Q pressed, exiting...\n");
                running = false;
                continue;
            }

            if (event.type == SDL_EVENT_KEY_DOWN &&
                (event.key.key == SDLK_F11 ||
                 (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)))) {
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(window, fullscreen);
                SDL_GL_MakeCurrent(window, gl_context);
                printf("[Game] fullscreen %s\n", fullscreen ? "on" : "off");
                continue;
            }

            if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                int new_w = event.window.data1;
                int new_h = event.window.data2;
                if (new_w > 0 && new_h > 0) {
                    win_w = new_w;
                    win_h = new_h;
                    SDL_GL_MakeCurrent(window, gl_context);
                    present_set_window(new_w, new_h);
                    screen_width = present_render_w();
                    screen_height = present_render_h();
                    ab_input_resize(new_w, new_h);
                    ab_input_set_render_size(screen_width, screen_height);
                    if (g_nativeResize) {
                        g_nativeResize(fake_env, thiz, screen_width, screen_height);
                    }
                }
                continue;
            }

            ab_input_handle(&event);
        }

        // Forward this frame's pointer/key events to the engine.
        AbInputEvent ievs[64];
        int nev = ab_input_poll(ievs, 64);
        if (frame_count < 25 && nev) {
            printf("[Game] frame %lu delivered %d input event(s) (type0=%d act0=%d id0=%d)\n",
                   (unsigned long)frame_count + 1, nev,
                   (int)ievs[0].type, (int)ievs[0].action, (int)ievs[0].pointer_id);
            fflush(stdout);
        }
        for (int i = 0; i < nev; i++) {
            const AbInputEvent* ie = &ievs[i];
            if (ie->type == AB_EV_POINTER) {
                if (g_nativeInput)
                    g_nativeInput(fake_env, thiz, ie->action, ie->x, ie->y, ie->pointer_id);
            } else if (ie->type == AB_EV_KEY && g_nativeKeyInput) {
                g_nativeKeyInput(fake_env, thiz, ie->keycode, ie->action, ie->unicode, ie->device_id);
            }
        }

        // Poll & Queue Audio
        audio_poll();

        // Deliver any fake-billing callback whose "network round trip" is done.
        iap_patch_poll();

        // Game Logic & Frame Rendering (Fusion renders inside nativeUpdate).
        // In a scaled mode the whole frame goes into our offscreen target.
        present_begin();

        bool engine_alive = true;
        if (g_nativeUpdate) {
            jboolean alive = g_nativeUpdate(fake_env, thiz);
            engine_alive = alive != 0;

            /* The engine returns false from its very first update in this host:
             * the lifecycle/state it reads lives on the Java side (NativeState
             * lives in NativeApplication.java, which we do not have), so a bare
             * false is not a shutdown request here. Treat it as one only after
             * the engine has reported true at least once -- true -> false is a
             * real "I was running and I am stopping now". ESC/Q, F11 and the
             * window's own close button remain the reliable ways out, and the
             * engine's own quit path still works once it ever reports true. */
            static int engine_reported_true = 0;
            if (alive) engine_reported_true = 1;
            else if (!engine_reported_true && !engine_alive) {
                static int notice = 0;
                if (!notice) {
                    notice = 1;
                    printf("[Game] nativeUpdate reports false; staying up until the engine "
                           "has reported true at least once (ESC/Q closes)\n");
                    fflush(stdout);
                }
                engine_alive = true;
            }

            static int last_reported = -1;
            if ((int)alive != last_reported) {
                printf("[Game] nativeUpdate frame %lu -> %d%s\n",
                       (unsigned long)frame_count + 1, (int)alive,
                       engine_alive ? "" : " (false)");
                fflush(stdout);
                last_reported = (int)alive;
            }
        }
        if (engine_alive && g_nativeRender) {
            jboolean drew = g_nativeRender(fake_env, thiz);
            static int last_drew = -1;
            if ((int)drew != last_drew) {
                printf("[Game] nativeRender frame %lu -> %d\n",
                       (unsigned long)frame_count + 1, (int)drew);
                fflush(stdout);
                last_drew = (int)drew;
            }
        }

        present_end();

        if (shot_path && (long)frame_count == shot_frame) present_capture(shot_path);

        // Present OpenGL frame
        SDL_GL_SwapWindow(window);

        frame_count++;
        if (frame_count <= 20 || frame_count % 30 == 0) {
            printf("[Game] Rendered frame %lu\n", (unsigned long)frame_count);
            fflush(stdout);
        }

        if (!engine_alive) {
            printf("[Game] Engine signalled exit (nativeUpdate returned false)\n");
            fflush(stdout);
            running = false;
        }

        if (fake_purchase && (long)frame_count == fake_purchase_frame) {
            iap_patch_request(strcmp(fake_purchase, "1") ? fake_purchase : nullptr);
        }

        if (autoclose && (long)frame_count >= autoclose && running) {
            printf("[Game] AB_AUTOCLOSE: closing the window after %ld frames\n",
                   (long)frame_count);
            fflush(stdout);
            SDL_Event quit;
            SDL_zero(quit);
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }

        // Cap to ~60 FPS
        uint64_t elapsed = SDL_GetTicks() - start_time;
        if (elapsed < 16) {
            SDL_Delay(16 - elapsed);
        }
    }

    printf("[Game] Exiting application cleanly (audio mixes: %lu, bytes: %lu)...\n",
           audio_mix_calls(), audio_bytes_mixed());
    fflush(stdout);

    ab_input_shutdown();

    /* Teardown order matters. The mixer runs on SDL's audio thread and calls
     * straight into the engine, so it has to be stopped BEFORE the engine starts
     * dismantling itself -- otherwise a mix lands in a half-freed mixer. */
    audio_shutdown();

    if (g_nativePause) {
        g_nativePause(fake_env, thiz);
    }

    if (g_nativeDeinit) {
        g_nativeDeinit(fake_env, thiz);
    }

    /* The engine keeps its own worker threads and has finished writing its save
     * by now. Tearing down EGL/SDL underneath those threads, and unmapping the
     * module out from under their code, is what used to turn "close the window"
     * into a crash -- so hand the process straight back to the kernel instead.
     * _exit() skips atexit handlers and every one of those teardown races. */
    printf("[Game] Application terminated cleanly.\n");
    fflush(stdout);
    fflush(stderr);
    _exit(0);
}
