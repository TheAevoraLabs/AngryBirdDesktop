#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <chrono>
#include <thread>
#include <filesystem>

#include "android/asset_manager.h"
#include "android/log.h"
#include "jni/jni_bridge.h"
#include "audio/audio.h"

namespace fs = std::filesystem;

// Global native function pointers
JNI_OnLoad_t g_JNI_OnLoad = nullptr;
nativeConfig_t g_nativeConfig = nullptr;
nativeGetPossibleOrientations_t g_nativeGetPossibleOrientations = nullptr;
nativeInit_t g_nativeInit = nullptr;
nativeDeinit_t g_nativeDeinit = nullptr;
nativePause_t g_nativePause = nullptr;
nativeResume_t g_nativeResume = nullptr;
nativeResize_t g_nativeResize = nullptr;
nativeUpdate_t g_nativeUpdate = nullptr;
nativeRender_t g_nativeRender = nullptr;
nativeFrameClear_t g_nativeFrameClear = nullptr;
nativeInput_t g_nativeInput = nullptr;
nativeKeyInput_t g_nativeKeyInput = nullptr;
nativeMixData_t g_nativeMixData = nullptr;

#include <exception>
#include <typeinfo>
#include <cxxabi.h>

static void* load_native_library(const char* libpath) {
    std::set_terminate([]() {
        std::exception_ptr p = std::current_exception();
        if (p) {
            try {
                std::rethrow_exception(p);
            } catch (const std::exception& e) {
                printf("[CRASH-DIAG] Uncaught std::exception: %s (type=%s)\n", e.what(), typeid(e).name());
            } catch (...) {
                const std::type_info* t = abi::__cxa_current_exception_type();
                printf("[CRASH-DIAG] Uncaught non-std::exception (type=%s)\n", t ? t->name() : "unknown");
            }
        } else {
            printf("[CRASH-DIAG] Terminate called with no active exception\n");
        }
        fflush(stdout);
        abort();
    });

    printf("[Loader] Loading %s ...\n", libpath);
    void* handle = dlopen(libpath, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        printf("[Loader] Error loading %s: %s\n", libpath, dlerror());
        return nullptr;
    }
    printf("[Loader] Successfully loaded %s\n", libpath);
    return handle;
}

#include <link.h>
#include <elf.h>
#include <funchook.h>

static uintptr_t g_lib_base = 0;
static const Elf32_Phdr* g_lib_phdrs = nullptr;
static int g_lib_phnum = 0;

static int dl_callback(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    if (info->dlpi_name && strstr(info->dlpi_name, "libAngryBirdsClassic.so")) {
        g_lib_base = (uintptr_t)info->dlpi_addr;
        g_lib_phdrs = info->dlpi_phdr;
        g_lib_phnum = info->dlpi_phnum;
    }
    return 0;
}

// The game is compiled against Bionic headers, whose `struct sigaction` is
// 16 bytes on 32-bit x86 instead of glibc's 140-byte layout. Calling glibc's
// sigaction directly smashes the stack (kernel writes 140 bytes into the
// 16-byte buffer). Redirect only the game's own GOT slots for sigaction /
// sigprocmask to Bionic-ABI wrappers, leaving SDL3 and everything else on the
// real glibc functions.
extern "C" int rovio_sigaction_compat(int signum, const void* act, void* oldact);
extern "C" int rovio_sigprocmask_compat(int how, const void* set, void* oldset);

static void* find_dynamic_entry(const Elf32_Dyn* dyn, size_t n, Elf32_Sword tag) {
    for (size_t i = 0; i < n; i++) {
        if (dyn[i].d_tag == tag) return (void*)(uintptr_t)dyn[i].d_un.d_ptr;
    }
    return nullptr;
}

// Relocation r_offset values are virtual addresses relative to the library
// base, so the in-memory GOT slot is simply base + r_offset.
static uintptr_t file_offset_to_addr(uintptr_t off) {
    return g_lib_base + off;
}

static void patch_got_symbol(const char* symname, void* replacement) {
    if (!g_lib_base || !g_lib_phdrs) {
        printf("[Hook] GOT patch: lib base/phdrs unavailable, skipping %s\n", symname);
        return;
    }

    const Elf32_Phdr* pt_dyn = nullptr;
    for (int i = 0; i < g_lib_phnum; i++) {
        if (g_lib_phdrs[i].p_type == PT_DYNAMIC) {
            pt_dyn = &g_lib_phdrs[i];
            break;
        }
    }
    if (!pt_dyn) {
        printf("[Hook] GOT patch: no PT_DYNAMIC, skipping %s\n", symname);
        return;
    }

    const Elf32_Dyn* dyn = (const Elf32_Dyn*)(g_lib_base + pt_dyn->p_vaddr);
    size_t dyn_n = pt_dyn->p_filesz / sizeof(Elf32_Dyn);

    Elf32_Rel* jmprel = (Elf32_Rel*)find_dynamic_entry(dyn, dyn_n, DT_JMPREL);
    size_t jmprelsz = (size_t)find_dynamic_entry(dyn, dyn_n, DT_PLTRELSZ);
    Elf32_Rel* rel = (Elf32_Rel*)find_dynamic_entry(dyn, dyn_n, DT_REL);
    size_t relsz = (size_t)find_dynamic_entry(dyn, dyn_n, DT_RELSZ);
    Elf32_Sym* symtab = (Elf32_Sym*)find_dynamic_entry(dyn, dyn_n, DT_SYMTAB);
    const char* strtab = (const char*)find_dynamic_entry(dyn, dyn_n, DT_STRTAB);
    if (!symtab || !strtab) {
        printf("[Hook] GOT patch: no symtab/strtab, skipping %s\n", symname);
        return;
    }

    auto scan = [&](Elf32_Rel* r, size_t sz) {
        size_t n = sz / sizeof(Elf32_Rel);
        for (size_t i = 0; i < n; i++) {
            int type = ELF32_R_TYPE(r[i].r_info);
            if (type != R_386_JMP_SLOT && type != R_386_GLOB_DAT) continue;
            unsigned symidx = ELF32_R_SYM(r[i].r_info);
            if (symidx == 0) continue;
            const char* name = strtab + symtab[symidx].st_name;
            if (strcmp(name, symname) != 0) continue;
            uintptr_t got_addr = file_offset_to_addr(r[i].r_offset);
            printf("[Hook] Redirecting %s GOT slot at 0x%lx to %p\n",
                   symname, (unsigned long)got_addr, replacement);
            // Defensive: the page could be made read-only by RELRO.
            uintptr_t page = got_addr & ~(uintptr_t)0xFFF;
            if (mprotect((void*)page, 0x1000, PROT_READ | PROT_WRITE) != 0) {
                printf("[Hook] Warning: mprotect failed for GOT page 0x%lx: %s\n",
                       (unsigned long)page, strerror(errno));
            }
            *(void**)got_addr = replacement;
        }
    };

    if (jmprel && jmprelsz) scan(jmprel, jmprelsz);
    if (rel && relsz) scan(rel, relsz);
}

typedef int (*lua_pcall_t)(void* L, int nargs, int nresults, int errfunc);
typedef const char* (*lua_tolstring_t)(void* L, int idx, size_t* len);
typedef int (*lua_load_t)(void* L, void* reader, void* data, const char* chunkname);

static lua_pcall_t g_orig_lua_pcall = nullptr;
static lua_tolstring_t g_lua_tolstring = nullptr;
static lua_load_t g_orig_lua_load = nullptr;

static int my_lua_pcall(void* L, int nargs, int nresults, int errfunc) {
    int res = g_orig_lua_pcall(L, nargs, nresults, errfunc);
    if (res != 0 && g_lua_tolstring) {
        const char* err = g_lua_tolstring(L, -1, nullptr);
        printf("[LUA-PCALL-FAILED] (code=%d, nargs=%d, nres=%d, errfunc=%d):\n%s\n",
               res, nargs, nresults, errfunc, err ? err : "unknown");
        fflush(stdout);
    }
    return res;
}

static int my_lua_load(void* L, void* reader, void* data, const char* chunkname) {
    printf("[LUA-LOAD] chunk: %s\n", chunkname ? chunkname : "(null)");
    fflush(stdout);
    int res = g_orig_lua_load(L, reader, data, chunkname);
    if (res != 0) {
        printf("[LUA-LOAD-FAILED] chunk: %s, code=%d\n", chunkname ? chunkname : "(null)", res);
        fflush(stdout);
    }
    return res;
}

// sub_3E862: void sub_3E862(lua_State* L, int errcode, const char* msg)
// This function wraps a Lua error into a C++ LuaException and throws it.
// On desktop we cannot handle uncaught C++ exceptions from the engine's Lua
// behavior system, so we hook this to just log and return silently.
typedef void (*lua_throw_error_t)(void* L, int errcode, const char* msg);
static lua_throw_error_t g_orig_lua_throw_error = nullptr;

static void my_lua_throw_error(void* L, int errcode, const char* msg) {
    printf("[Hook] Suppressed LuaException throw (code=%d): %s\n",
           errcode, msg ? msg : "(null)");
    fflush(stdout);
    (void)L;
}

typedef void (*glDrawArrays_t)(GLenum mode, GLint first, GLsizei count);
typedef void (*glDrawElements_t)(GLenum mode, GLsizei count, GLenum type, const void* indices);
typedef ssize_t (*read_t)(int fd, void* buf, size_t count);

static glDrawArrays_t g_orig_glDrawArrays = nullptr;
static glDrawElements_t g_orig_glDrawElements = nullptr;
static read_t g_orig_read = nullptr;
static uint64_t g_draw_call_count = 0;

static void my_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    g_draw_call_count++;
    if (g_orig_glDrawArrays) g_orig_glDrawArrays(mode, first, count);
}

static void my_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    g_draw_call_count++;
    if (g_orig_glDrawElements) g_orig_glDrawElements(mode, count, type, indices);
}

static ssize_t my_read(int fd, void* buf, size_t count) {
    if (fd == 0) {
        return 0; // EOF on stdin so engine CLI / console never hangs
    }
    if (g_orig_read) {
        return g_orig_read(fd, buf, count);
    }
    return syscall(SYS_read, fd, buf, count);
}

static void install_hooks() {
    dl_iterate_phdr(dl_callback, nullptr);
    if (!g_lib_base) {
        printf("[Hook] libAngryBirdsClassic.so base not found!\n");
        return;
    }
    printf("[Hook] libAngryBirdsClassic.so base: 0x%lx\n", (unsigned long)g_lib_base);

    g_orig_lua_pcall = (lua_pcall_t)(g_lib_base + 0x8d66f0);
    g_lua_tolstring = (lua_tolstring_t)(g_lib_base + 0x8d49f0);
    g_orig_lua_load = (lua_load_t)(g_lib_base + 0x8d6850);
    // Redirect the game's sigaction/sigprocmask GOT slots to Bionic-ABI
    // wrappers BEFORE any game code runs (nativeInit installs signal handlers).
    patch_got_symbol("sigaction", (void*)rovio_sigaction_compat);
    patch_got_symbol("sigprocmask", (void*)rovio_sigprocmask_compat);

    g_orig_lua_throw_error = (lua_throw_error_t)(g_lib_base + 0x3e862);
    g_orig_read = (read_t)dlsym(RTLD_DEFAULT, "read");
    g_orig_glDrawArrays = (glDrawArrays_t)dlsym(RTLD_DEFAULT, "glDrawArrays");
    g_orig_glDrawElements = (glDrawElements_t)dlsym(RTLD_DEFAULT, "glDrawElements");

    funchook_t *funchook = funchook_create();
    if (funchook) {
        funchook_prepare(funchook, (void**)&g_orig_lua_pcall, (void*)my_lua_pcall);
        funchook_prepare(funchook, (void**)&g_orig_lua_load, (void*)my_lua_load);
        funchook_prepare(funchook, (void**)&g_orig_lua_throw_error, (void*)my_lua_throw_error);
        if (g_orig_read) funchook_prepare(funchook, (void**)&g_orig_read, (void*)my_read);
        if (g_orig_glDrawArrays) funchook_prepare(funchook, (void**)&g_orig_glDrawArrays, (void*)my_glDrawArrays);
        if (g_orig_glDrawElements) funchook_prepare(funchook, (void**)&g_orig_glDrawElements, (void*)my_glDrawElements);
        int rv = funchook_install(funchook, 0);
        printf("[Hook] funchook_install result: %d\n", rv);
        printf("[Hook]   lua_pcall    -> %p\n", (void*)g_orig_lua_pcall);
        printf("[Hook]   lua_load     -> %p\n", (void*)g_orig_lua_load);
        printf("[Hook]   lua_throw    -> %p\n", (void*)g_orig_lua_throw_error);
        printf("[Hook]   read         -> %p\n", (void*)g_orig_read);
    }
}

static bool resolve_symbols(void* handle) {
    if (!handle) return false;

    #define RESOLVE(name, type) \
        g_##name = (type)dlsym(handle, "Java_com_rovio_fusion_NativeApplication_" #name); \
        if (!g_##name) { \
            g_##name = (type)dlsym(handle, #name); \
        } \
        printf("[Loader] Symbol %s: %p\n", #name, (void*)g_##name);

    g_JNI_OnLoad = (JNI_OnLoad_t)dlsym(handle, "JNI_OnLoad");
    RESOLVE(nativeConfig, nativeConfig_t);
    RESOLVE(nativeGetPossibleOrientations, nativeGetPossibleOrientations_t);
    RESOLVE(nativeInit, nativeInit_t);
    RESOLVE(nativeDeinit, nativeDeinit_t);
    RESOLVE(nativePause, nativePause_t);
    RESOLVE(nativeResume, nativeResume_t);
    RESOLVE(nativeResize, nativeResize_t);
    RESOLVE(nativeUpdate, nativeUpdate_t);
    RESOLVE(nativeRender, nativeRender_t);
    RESOLVE(nativeFrameClear, nativeFrameClear_t);

    g_nativeInput = (nativeInput_t)dlsym(handle, "Java_com_rovio_fusion_MyInputHandler_nativeInput");
    g_nativeKeyInput = (nativeKeyInput_t)dlsym(handle, "Java_com_rovio_fusion_MyInputHandler_nativeKeyInput");
    g_nativeMixData = (nativeMixData_t)dlsym(handle, "Java_com_rovio_fusion_AudioOutput_nativeMixData");
    g_onVideoEnded = (onVideoEnded_t)dlsym(handle, "Java_com_rovio_fusion_VideoPlayerBridge_onVideoEnded");

    printf("[Loader] Symbol nativeInput: %p\n", (void*)g_nativeInput);
    printf("[Loader] Symbol nativeKeyInput: %p\n", (void*)g_nativeKeyInput);
    printf("[Loader] Symbol nativeMixData: %p\n", (void*)g_nativeMixData);
    printf("[Loader] Symbol onVideoEnded: %p\n", (void*)g_onVideoEnded);

    install_hooks();

    return (g_nativeInit && g_nativeUpdate && g_nativeRender);
}

int main(int argc, char* argv[]) {
    // Ensure LD_LIBRARY_PATH includes bin and 32-bit library directories
    const char* cur_ld = getenv("LD_LIBRARY_PATH");
    const char* reexec_marker = getenv("__AB_REEXEC");
    if (!reexec_marker) {
        std::string new_ld = "bin:../bin:angry-birds-classic-8-0-3/lib/x86";
        if (cur_ld) {
            new_ld += ":";
            new_ld += cur_ld;
        }
        setenv("LD_LIBRARY_PATH", new_ld.c_str(), 1);
        setenv("__AB_REEXEC", "1", 1);
        printf("[Bootstrap] Setting LD_LIBRARY_PATH=%s and launching...\n", new_ld.c_str());
        fflush(stdout);
        execv("/proc/self/exe", argv);
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=========================================\n");
    printf("       Angry Birds Desktop (x86)        \n");
    printf("=========================================\n");

    // 1. Ensure save and assets directory exist
    fs::create_directories("save");
    
    // Set base path for AAssetManager
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

    // Request OpenGL ES 2.0 compatible context
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    // The window manager on some desktops sends spurious close requests while
    // the engine is busy loading (long synchronous Lua loads), and SDL3 by
    // default turns an unhandled close request into SDL_EVENT_QUIT. The game
    // owns its own lifecycle, so ignore WM close pokes; ESC still exits.
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");

    int window_width = 1280;
    int window_height = 720;

    SDL_Window* window = SDL_CreateWindow(
        "Angry Birds Classic",
        window_width,
        window_height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        printf("[SDL3] Failed to create window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        printf("[SDL3] Failed to create OpenGL context: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_GL_SetSwapInterval(1); // Enable VSync

    // 3. Initialize Audio
    audio_init(44100, 2);

    // 4. Initialize JNI Bridge
    jni_bridge_init();

    // Preload Android and Bionic shims
    const char* preloads[] = {
        "bin/libc.so", "/lib/libc.so.6",
        "bin/libm.so", "/lib/libm.so.6",
        "bin/libstdc++.so", "/usr/lib/libstdc++.so.6",
        "bin/liblog.so", "./liblog.so", "liblog.so",
        "bin/libandroid.so", "./libandroid.so", "libandroid.so",
        "bin/libjs.so", "./libjs.so"
    };
    for (const char* p : preloads) {
        if (fs::exists(p)) {
            dlopen(p, RTLD_NOW | RTLD_GLOBAL);
        }
    }

    // 5. Load Native Library
    const char* libpaths[] = {
        "bin/libAngryBirdsClassic.so",
        "./libAngryBirdsClassic.so",
        "angry-birds-classic-8-0-3/lib/x86/libAngryBirdsClassic.so",
        "../bin/libAngryBirdsClassic.so"
    };

    void* lib_handle = nullptr;
    for (const char* path : libpaths) {
        if (fs::exists(path)) {
            lib_handle = load_native_library(path);
            if (lib_handle) break;
        }
    }

    if (!lib_handle) {
        printf("[Error] Could not load libAngryBirdsClassic.so from any searched path.\n");
        SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    if (!resolve_symbols(lib_handle)) {
        printf("[Error] Failed to resolve required native symbols!\n");
        SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    JNIEnv* env = jni_get_env();
    JavaVM* vm = jni_get_java_vm();

    // 6. JNI OnLoad
    if (g_JNI_OnLoad) {
        printf("[Game] Calling JNI_OnLoad...\n");
        g_JNI_OnLoad(vm, nullptr);
    }

    // 7. Configure and Initialize Engine
    if (g_nativeConfig) {
        printf("[Game] Calling nativeConfig...\n");
        jstring cfg = env->NewStringUTF("./save");
        g_nativeConfig(env, nullptr, cfg);
    }

    printf("[Game] Initializing engine with resolution %dx%d...\n", window_width, window_height);
    fflush(stdout);
    if (g_nativeInit) {
        g_nativeInit(env, nullptr, window_width, window_height);
    }
    printf("[Game] nativeInit completed successfully!\n");
    fflush(stdout);

    if (g_nativeResume) {
        printf("[Game] Resuming native application...\n");
        fflush(stdout);
        g_nativeResume(env, nullptr);
        printf("[Game] nativeResume completed successfully!\n");
        fflush(stdout);
    }

    // 8. Main Application Loop
    bool running = true;
    bool mouse_down = false;
    SDL_Event event;
    uint64_t frame_count = 0;

    printf("[Game] Entering main render loop...\n");
    fflush(stdout);

    while (running) {
        uint64_t start_time = SDL_GetTicks();

        // Poll Events
        while (SDL_PollEvent(&event)) {
            if (event.type != SDL_EVENT_MOUSE_MOTION) {
                printf("[Event] type=%d ts=%llu win=%u data1=%d data2=%d\n", (int)event.type,
                       (unsigned long long)event.common.timestamp,
                       (unsigned int)event.window.windowID,
                       event.window.data1, event.window.data2);
                fflush(stdout);
            }
            switch (event.type) {
                case SDL_EVENT_QUIT: {
                    printf("[Game] Received SDL_EVENT_QUIT\n");
                    const char* sdl_err = SDL_GetError();
                    printf("[Game] SDL_GetError: %s\n", sdl_err ? sdl_err : "(null)");
                    const unsigned char* raw = (const unsigned char*)&event;
                    printf("[Game] raw event: ");
                    for (int i = 0; i < 24; i++) printf("%02x ", raw[i]);
                    printf("\n");
                    fflush(stdout);
                    running = false;
                    break;
                }

                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    printf("[Game] WM close requested (ignored; press ESC to exit)\n");
                    break;

                case SDL_EVENT_WINDOW_RESIZED: {
                    int new_w = event.window.data1;
                    int new_h = event.window.data2;
                    if (new_w > 0 && new_h > 0) {
                        window_width = new_w;
                        window_height = new_h;
                        if (g_nativeResize) {
                            g_nativeResize(env, nullptr, new_w, new_h);
                        }
                    }
                    break;
                }

                case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        mouse_down = true;
                        float x = event.button.x;
                        float y = event.button.y;
                        if (g_nativeInput) {
                            g_nativeInput(env, nullptr, 0 /* ACTION_DOWN */, x, y, 0);
                        }
                    }
                    break;
                }

                case SDL_EVENT_MOUSE_BUTTON_UP: {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        mouse_down = false;
                        float x = event.button.x;
                        float y = event.button.y;
                        if (g_nativeInput) {
                            g_nativeInput(env, nullptr, 1 /* ACTION_UP */, x, y, 0);
                        }
                    }
                    break;
                }

                case SDL_EVENT_MOUSE_MOTION: {
                    if (mouse_down) {
                        float x = event.motion.x;
                        float y = event.motion.y;
                        if (g_nativeInput) {
                            g_nativeInput(env, nullptr, 2 /* ACTION_MOVE */, x, y, 0);
                        }
                    }
                    break;
                }

                case SDL_EVENT_KEY_DOWN: {
                    if (event.key.key == SDLK_ESCAPE) {
                        printf("[Game] ESC pressed, exiting...\n");
                        running = false;
                    }
                    if (g_nativeKeyInput) {
                        g_nativeKeyInput(env, nullptr, event.key.key, 0, 0 /* ACTION_DOWN */, 0);
                    }
                    break;
                }

                case SDL_EVENT_KEY_UP: {
                    if (g_nativeKeyInput) {
                        g_nativeKeyInput(env, nullptr, event.key.key, 0, 1 /* ACTION_UP */, 0);
                    }
                    break;
                }

                default:
                    break;
            }
        }

        // Tick Audio
        audio_tick();

        // Tick Game Logic
        if (g_nativeUpdate) {
            g_nativeUpdate(env, nullptr);
        }

        // Render Frame
        if (g_nativeRender) {
            g_nativeRender(env, nullptr);
        }

        // Swap OpenGL Buffer
        SDL_GL_SwapWindow(window);

        frame_count++;
        if (frame_count <= 20 || frame_count % 30 == 0) {
            printf("[Game] Rendered %llu frames (draw calls: %llu)\n",
                   (unsigned long long)frame_count, (unsigned long long)g_draw_call_count);
            fflush(stdout);
        }

        // Maintain ~60 FPS
        uint64_t elapsed = SDL_GetTicks() - start_time;
        if (elapsed < 16) {
            SDL_Delay(16 - elapsed);
        }
    }

    printf("[Game] Shutting down...\n");
    if (g_nativePause) {
        g_nativePause(env, nullptr);
    }
    if (g_nativeDeinit) {
        g_nativeDeinit(env, nullptr);
    }

    audio_shutdown();
    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("[Game] Clean exit.\n");
    return 0;
}
