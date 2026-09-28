/* main_friends.cpp -- driver for Angry Birds Friends 1.0.0 (Fusion ~3.x).
 *
 * Why this is a separate driver rather than a flag in main.cpp: the two titles
 * speak different engine lifecycles, and Classic's bring-up is delicate enough
 * that it should not grow conditionals for a game it does not know about. What
 * is genuinely shared (ELF loader, Bionic shims, JNI bridge, audio, input,
 * presentation, crash handler) is shared; only the shell differs.
 *
 * Angry Birds Friends differs from Classic in these ways (all read off the
 * binary, see bin/libAngryBirdsFriends.so):
 *
 *   - No com.rovio.fusion.NativeApplication and no MyInputHandler. Every entry
 *     point hangs off com.rovio.fusion.MyRenderer (with a MyLegacyRenderer
 *     twin). There is no nativeConfig and no nativeRender.
 *   - The writable-data directory arrives as the fifth argument of nativeInit:
 *       nativeInit(JNIEnv*, jobject, int width, int height, jstring dataPath)
 *   - nativeUpdate is void:
 *       _Z12nativeUpdateP7_JNIEnvP8_jobject     (no return value)
 *     so its result must never be read as a shutdown signal.
 *   - nativeKeyInput takes three ints, not four:
 *       _Z14nativeKeyInputP7_JNIEnvP8_jobjectiii
 *   - nativeInput keeps Classic's convention, and it is worth spelling out
 *     because cdecl makes argument order load-bearing:
 *       _Z11nativeInputP7_JNIEnvP8_jobjectiffi  ->  (action, x, y, pointerId)
 *
 * Every bring-up step is a numbered stage, and AB_STOP_AFTER=<n> makes the
 * driver exit cleanly once stage n has finished -- so how far the port gets can
 * be bisected without a debugger.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengles2.h>

#include "loader/so_util.h"
#include "jni/jni.h"
#include "jni/jni_fake.h"
#include "audio/audio.h"
#include "input/input.h"
#include "render/present.h"
#include "crash/crash.h"
#include "common/game_config.h"
#include "common/game_profile.h"
#include "common/bionic_dynlib.h"
#include "main_games.h"
#include "android/asset_manager.h"
#include "android/log.h"

#include <funchook.h>

namespace fs = std::filesystem;

/* ------------------------------------------------------------------ *
 * Engine entry points for this title.
 *
 * These are deliberately NOT the same typedefs main.cpp uses: nativeInit has an
 * extra jstring, nativeUpdate returns nothing, and nativeKeyInput is one int
 * short. Getting an arity wrong here corrupts the callee's view of its own
 * stack, so each one is pinned to the mangled signature printed by
 *   nm -D --defined-only bin/libAngryBirdsFriends.so | c++filt | grep native
 * ------------------------------------------------------------------ */
typedef jint (*JNI_OnLoad_t)(JavaVM, void*);
typedef void (*nativeInit_t)(JNIEnv env, jobject thiz, jint width, jint height, jstring data_path);
typedef void (*nativeVoid_t)(JNIEnv env, jobject thiz);
typedef void (*nativeResize_t)(JNIEnv env, jobject thiz, jint width, jint height);
typedef void (*nativeUpdate_t)(JNIEnv env, jobject thiz);          /* void! */
typedef void (*nativeInput_t)(JNIEnv env, jobject thiz, jint action, jfloat x, jfloat y, jint pointer_id);
typedef void (*nativeKeyInput_t)(JNIEnv env, jobject thiz, jint key_code, jint action, jint unicode);
typedef void (*nativeLoadFromUrl_t)(JNIEnv env, jobject thiz, jstring url);
typedef jint (*nativeOrientations_t)(JNIEnv env, jobject thiz);

static so_module g_game_mod;

static JNI_OnLoad_t          g_JNI_OnLoad    = nullptr;
static nativeInit_t          g_nativeInit    = nullptr;
static nativeVoid_t          g_nativeDeinit  = nullptr;
static nativeVoid_t          g_nativePause   = nullptr;
static nativeVoid_t          g_nativeResume  = nullptr;
static nativeResize_t        g_nativeResize  = nullptr;
static nativeUpdate_t        g_nativeUpdate  = nullptr;
static nativeInput_t         g_nativeInput   = nullptr;
static nativeKeyInput_t      g_nativeKeyInput = nullptr;
static nativeLoadFromUrl_t   g_nativeLoadFromUrl = nullptr;
static nativeOrientations_t  g_nativeOrientations = nullptr;
static void*                 g_nativeMixData_addr = nullptr;

/* ------------------------------------------------------------------ *
 * Staged bring-up bookkeeping
 * ------------------------------------------------------------------ */
static int g_stage = 0;
static long g_stop_after = -1;   /* AB_STOP_AFTER=<n> */

static uintptr_t entry_sym(so_module* mod, const ab_entry* e) {
    if (!mod || !e) return 0;
    if (e->offset) return (uintptr_t)(mod->base + e->offset);
    if (!e->symbol) return 0;
    return so_symbol(mod, e->symbol);
}

static const char* stage_name(int n) {
    switch (n) {
        case 0:  return "startup";
        case 1:  return "filesystem + config";
        case 2:  return "asset root";
        case 3:  return "SDL + GLES2 context";
        case 4:  return "presentation + input";
        case 5:  return "fake JNI";
        case 6:  return "ELF load";
        case 7:  return "relocations";
        case 8:  return "dynamic imports";
        case 9:  return "static initializers";
        case 10: return "entry-point resolution";
        case 11: return "runtime hooks";
        case 12: return "audio mixer";
        case 13: return "JNI_OnLoad";
        case 14: return "nativeInit";
        case 15: return "nativeResize/nativeResume";
        case 16: return "main loop";
        default: return "?";
    }
}

/* Marks stage n complete and reports it. Returns false when the run was asked
 * to stop here (AB_STOP_AFTER), which lets the caller unwind normally. */
static bool stage_done(int n) {
    g_stage = n;
    printf("[Friends][stage %2d] ok: %s\n", n, stage_name(n));
    fflush(stdout);
    if (g_stop_after >= 0 && n >= g_stop_after) {
        printf("[Friends] AB_STOP_AFTER=%ld reached after stage %d (%s); stopping here.\n",
               g_stop_after, n, stage_name(n));
        fflush(stdout);
        return false;
    }
    return true;
}

static void stage_fail(int n, const char* what) {
    printf("[Friends][stage %2d] FAILED: %s (%s)\n", n, stage_name(n), what);
    fflush(stdout);
}

/* ------------------------------------------------------------------ *
 * Lua diagnostics.
 *
 * Friends exports the Lua VM (lua_pcall / lua_load / lua_tolstring are plain C
 * symbols in its dynamic table), so unlike Classic we can hook the real
 * functions by name instead of by module offset.
 *
 * The traceback is intentionally shallow: it reports the error string only.
 * Walking Lua's internal CallInfo would assume class- (and build-) specific
 * struct offsets, and a wrong offset here turns a diagnostic into a crash.
 * ------------------------------------------------------------------ */
typedef int (*lua_pcall_t)(void* L, int nargs, int nresults, int errfunc);
typedef int (*lua_load_t)(void* L, void* reader, void* data, const char* chunkname);
typedef const char* (*lua_tolstring_t)(void* L, int idx, size_t* len);

static lua_pcall_t     g_orig_lua_pcall     = nullptr;
static lua_load_t      g_orig_lua_load      = nullptr;
static lua_tolstring_t g_lua_tolstring      = nullptr;

static int my_lua_pcall(void* L, int nargs, int nresults, int errfunc) {
    int res = g_orig_lua_pcall(L, nargs, nresults, errfunc);
    if (res != 0) {
        const char* err = g_lua_tolstring ? g_lua_tolstring(L, -1, nullptr) : "(no tolstring)";
        printf("[Friends][Lua] pcall error %d -> %s\n", res, err ? err : "(null)");
        fflush(stdout);
    }
    return res;
}

static int my_lua_load(void* L, void* reader, void* data, const char* chunkname) {
    printf("[Friends][Lua] loading chunk: %s\n", chunkname ? chunkname : "(null)");
    fflush(stdout);
    return g_orig_lua_load(L, reader, data, chunkname);
}

static void install_runtime_hooks(so_module* mod, const ab_game_profile* prof) {
    if (!mod || !mod->base) return;

    g_orig_lua_pcall  = (lua_pcall_t)ab_hook_resolve(&prof->hk_lua_pcall, mod, mod->base);
    g_orig_lua_load   = (lua_load_t)ab_hook_resolve(&prof->hk_lua_load, mod, mod->base);
    g_lua_tolstring   = (lua_tolstring_t)ab_hook_resolve(&prof->hk_lua_tolstring, mod, mod->base);

    printf("[Friends][Hooks] lua_pcall=%p lua_load=%p lua_tolstring=%p\n",
           (void*)g_orig_lua_pcall, (void*)g_orig_lua_load, (void*)g_lua_tolstring);
    fflush(stdout);

    if (!g_orig_lua_pcall && !g_orig_lua_load) {
        printf("[Friends][Hooks] no Lua symbols resolved; continuing without Lua tracing\n");
        return;
    }

    funchook_t* funchook = funchook_create();
    if (!funchook) {
        printf("[Friends][Hooks] funchook_create failed; continuing without hooks\n");
        return;
    }
    if (g_orig_lua_pcall) funchook_prepare(funchook, (void**)&g_orig_lua_pcall, (void*)my_lua_pcall);
    if (g_orig_lua_load)  funchook_prepare(funchook, (void**)&g_orig_lua_load, (void*)my_lua_load);
    int rv = funchook_install(funchook, 0);
    printf("[Friends][Hooks] funchook_install -> %d\n", rv);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
int ab_run_friends(int argc, char* argv[]) {
    const ab_game_profile* prof = ab_game_profile_get(AB_GAME_FRIENDS);
    if (!prof) return 2;
    (void)argc;   /* the dispatcher already consumed the game-selection flags */

    if (const char* s = getenv("AB_STOP_AFTER")) g_stop_after = atol(s);

    printf("=========================================\n");
    printf("   Angry Birds Desktop (In-Memory x86)   \n");
    printf("   profile: %-30s\n", prof->name);
    printf("   driver:  main_friends.cpp\n");
    printf("=========================================\n");
    fflush(stdout);

    if (!stage_done(0)) return 0;

    /* ---- stage 1: filesystem + config ------------------------------------ */
    fs::create_directories(prof->save_dir);
    fs::create_directories(prof->layout_dir);
    config_load(prof->config_name);
    screen_width = config.width;
    screen_height = config.height;

    crash_init(argv && argv[0] ? argv[0] : nullptr, prof->save_dir);

    printf("[Friends][Config] %s fullscreen=%d vsync=%d scaler=%s renderScale=%d%%\n",
           (config.width == 0) ? "desktop" : "fixed", config.fullscreen, config.vsync,
           config.scaler, config.render_scale);
    printf("[Friends] save dir: %s   data path: %s\n", prof->save_dir, prof->data_path);
    /* The Classic save patchers understand Classic's RCS wallet and settings.lua
     * only; Friends keeps its progress in a different layout, so they are
     * skipped rather than allowed to scribble on the wrong files. */
    printf("[Friends] classic save patches: %s\n",
           prof->has_classic_save_patches ? "enabled" : "not applicable (skipped)");
    if (!stage_done(1)) return 0;

    /* ---- stage 2: asset root --------------------------------------------- */
    {
        const char* asset_root = nullptr;
        for (int i = 0; i < AB_MAX_ASSET_CANDIDATES && prof->asset_candidates[i]; i++) {
            printf("[Friends][Assets] probing %s ... %s\n", prof->asset_candidates[i],
                   fs::exists(prof->asset_candidates[i]) ? "found" : "missing");
            if (!asset_root && fs::exists(prof->asset_candidates[i]))
                asset_root = prof->asset_candidates[i];
        }
        if (asset_root) {
            AAssetManager_setBasePath(asset_root);
            printf("[Friends][Assets] using %s\n", asset_root);
        } else {
            printf("[Friends][Assets] WARNING: no Friends asset root found; the engine "
                   "will see an empty asset manager\n");
        }
    }
    if (!stage_done(2)) return 0;

    /* ---- stage 3: SDL + GLES2 context ------------------------------------ */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        stage_fail(3, SDL_GetError());
        return 1;
    }
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        printf("[Friends][SDL] gamepad subsystem unavailable (%s)\n", SDL_GetError());

    if (screen_width <= 0 || screen_height <= 0) {
        const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (dm && dm->w > 0 && dm->h > 0) {
            if (screen_width <= 0) screen_width = dm->w;
            if (screen_height <= 0) screen_height = dm->h;
        } else {
            if (screen_width <= 0) screen_width = 1280;
            if (screen_height <= 0) screen_height = 720;
        }
        printf("[Friends][SDL] desktop resolution %dx%d\n", screen_width, screen_height);
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow(prof->display_name, screen_width, screen_height,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        stage_fail(3, SDL_GetError());
        SDL_Quit();
        return 1;
    }

    bool fullscreen = config.fullscreen != 0;
    if (fullscreen) SDL_SetWindowFullscreen(window, true);

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        stage_fail(3, SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    if (!SDL_GL_MakeCurrent(window, gl_context)) {
        stage_fail(3, SDL_GetError());
        SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(config.vsync ? 1 : 0);

    printf("[Friends][GL] %s | %s\n",
           (const char*)glGetString(GL_VERSION),
           (const char*)glGetString(GL_RENDERER));
    fflush(stdout);
    if (!stage_done(3)) {
        SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    /* ---- stage 4: presentation + input ----------------------------------- */
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
    if (!stage_done(4)) return 0;

    /* ---- stage 5: fake JNI ----------------------------------------------- */
    jni_init();
    void* thiz = jni_make_thiz();
    if (!stage_done(5)) return 0;

    /* ---- stage 6: ELF load ----------------------------------------------- */
    const char* selected_path = nullptr;
    for (int i = 0; i < AB_MAX_SO_CANDIDATES && prof->so_candidates[i]; i++) {
        if (fs::exists(prof->so_candidates[i])) {
            selected_path = prof->so_candidates[i];
            break;
        }
    }
    if (!selected_path) {
        stage_fail(6, "no libAngryBirdsFriends.so candidate exists");
        printf("[Friends] tried:\n");
        for (int i = 0; i < AB_MAX_SO_CANDIDATES && prof->so_candidates[i]; i++)
            printf("           %s\n", prof->so_candidates[i]);
        return 1;
    }

    printf("[Friends][Loader] mapping %s\n", selected_path);
    if (so_load(&g_game_mod, selected_path) != 0) {
        stage_fail(6, "so_load failed (ELF parse/map)");
        return 1;
    }
    printf("[Friends][Loader] base=%p size=0x%zx\n",
           (void*)g_game_mod.base, g_game_mod.size);
    fflush(stdout);

    crash_set_module(selected_path, (uintptr_t)g_game_mod.base, g_game_mod.size);
    if (!stage_done(6)) return 0;

    /* ---- stage 7: relocations -------------------------------------------- */
    so_relocate(&g_game_mod);
    if (!stage_done(7)) return 0;

    /* ---- stage 8: dynamic imports ---------------------------------------- */
    {
        size_t dynlib_count = 0;
        const so_default_dynlib* dynlibs = ab_bionic_dynlib(&dynlib_count);
        printf("[Friends][Loader] resolving %zu Bionic imports\n", dynlib_count);
        so_resolve(&g_game_mod, dynlibs, (int)dynlib_count);
    }
    if (!stage_done(8)) return 0;

    /* ---- stage 9: static initializers ------------------------------------ */
    so_initialize(&g_game_mod);
    if (!stage_done(9)) return 0;

    /* ---- stage 10: entry-point resolution -------------------------------- */
    g_JNI_OnLoad          = (JNI_OnLoad_t)entry_sym(&g_game_mod, &prof->jni_on_load);
    g_nativeInit          = (nativeInit_t)entry_sym(&g_game_mod, &prof->init);
    g_nativeDeinit        = (nativeVoid_t)entry_sym(&g_game_mod, &prof->deinit);
    g_nativePause         = (nativeVoid_t)entry_sym(&g_game_mod, &prof->pause);
    g_nativeResume        = (nativeVoid_t)entry_sym(&g_game_mod, &prof->resume);
    g_nativeResize        = (nativeResize_t)entry_sym(&g_game_mod, &prof->resize);
    g_nativeUpdate        = (nativeUpdate_t)entry_sym(&g_game_mod, &prof->update);
    g_nativeInput         = (nativeInput_t)entry_sym(&g_game_mod, &prof->input);
    g_nativeKeyInput      = (nativeKeyInput_t)entry_sym(&g_game_mod, &prof->key_input);
    g_nativeLoadFromUrl   = (nativeLoadFromUrl_t)entry_sym(&g_game_mod, &prof->load_from_url);
    g_nativeOrientations  = (nativeOrientations_t)entry_sym(&g_game_mod, &prof->get_orientations);
    g_nativeMixData_addr  = (void*)entry_sym(&g_game_mod, &prof->mix_data);

    printf("[Friends][Engine] entry points:\n");
    printf("  JNI_OnLoad        %s\n", g_JNI_OnLoad ? "ok" : "--");
    printf("  nativeInit        %p\n", (void*)g_nativeInit);
    printf("  nativeDeinit      %p\n", (void*)g_nativeDeinit);
    printf("  nativePause       %p\n", (void*)g_nativePause);
    printf("  nativeResume      %p\n", (void*)g_nativeResume);
    printf("  nativeResize      %p\n", (void*)g_nativeResize);
    printf("  nativeUpdate      %p\n", (void*)g_nativeUpdate);
    printf("  nativeInput       %p\n", (void*)g_nativeInput);
    printf("  nativeKeyInput    %p\n", (void*)g_nativeKeyInput);
    printf("  nativeLoadFromUrl %p\n", (void*)g_nativeLoadFromUrl);
    printf("  nativeGetOrient.  %p\n", (void*)g_nativeOrientations);
    printf("  nativeMixData     %p\n", g_nativeMixData_addr);
    fflush(stdout);

    if (!g_nativeInit || !g_nativeUpdate) {
        stage_fail(10, "core symbols missing (need nativeInit + nativeUpdate)");
        return 1;
    }
    if (!stage_done(10)) return 0;

    /* ---- stage 11: hooks ------------------------------------------------- */
    install_runtime_hooks(&g_game_mod, prof);
    if (!stage_done(11)) return 0;

    /* ---- stage 12: audio mixer ------------------------------------------- */
    if (g_nativeMixData_addr) {
        const char* audio_env = getenv("AB_AUDIO");
        if (audio_env && audio_env[0] == '0') {
            printf("[Friends][Audio] disabled by AB_AUDIO=0\n");
        } else {
            audio_set_mixer(g_nativeMixData_addr, thiz);
            printf("[Friends][Audio] mixer registered\n");
        }
    } else {
        printf("[Friends][Audio] no nativeMixData -- running silent\n");
    }
    if (!stage_done(12)) return 0;

    /* ---- stage 13: JNI_OnLoad -------------------------------------------- */
    if (g_JNI_OnLoad) {
        printf("[Friends][Game] calling JNI_OnLoad...\n");
        fflush(stdout);
        jint ver = g_JNI_OnLoad(fake_vm, nullptr);
        printf("[Friends][Game] JNI_OnLoad returned 0x%x\n", (unsigned)ver);
        fflush(stdout);
    } else {
        printf("[Friends][Game] no JNI_OnLoad exported; skipping\n");
    }
    if (!stage_done(13)) return 0;

    /* ---- stage 14: nativeInit -------------------------------------------
     * Friends takes the writable-data path here rather than through a separate
     * nativeConfig call, and it is the last argument, after width and height. */
    {
        jstring data_path = jni_make_string(prof->data_path);
        printf("[Friends][Game] nativeInit(%d, %d, \"%s\")...\n",
               screen_width, screen_height, prof->data_path);
        fflush(stdout);
        g_nativeInit(fake_env, thiz, screen_width, screen_height, data_path);
        printf("[Friends][Game] nativeInit returned\n");
        fflush(stdout);
    }
    if (!stage_done(14)) return 0;

    /* ---- stage 15: resize + resume --------------------------------------- */
    if (g_nativeResize) {
        printf("[Friends][Game] nativeResize(%d, %d)...\n", screen_width, screen_height);
        fflush(stdout);
        g_nativeResize(fake_env, thiz, screen_width, screen_height);
    }
    if (g_nativeResume) {
        printf("[Friends][Game] nativeResume()...\n");
        fflush(stdout);
        g_nativeResume(fake_env, thiz);
    }
    printf("[Friends][Game] engine is up\n");
    fflush(stdout);
    if (!stage_done(15)) return 0;

    /* ---- stage 16: main loop --------------------------------------------- */
    bool running = true;
    SDL_Event event;
    uint64_t frame_count = 0;

    const char* shot_path = getenv("AB_SCREENSHOT");
    long shot_frame = 600;
    if (const char* s = getenv("AB_SCREENSHOT_FRAME")) {
        long v = atol(s);
        if (v > 0) shot_frame = v;
    }
    long autoclose = 0;
    if (const char* s = getenv("AB_AUTOCLOSE")) {
        autoclose = atol(s);
        if (autoclose < 1) autoclose = 1;
    }

    printf("[Friends][Game] entering main loop\n");
    fflush(stdout);
    stage_done(16);

    while (running && !jni_quit_requested) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                printf("[Friends][Game] SDL_EVENT_QUIT\n");
                running = false;
                continue;
            }
            if (event.type == SDL_EVENT_KEY_DOWN &&
                (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q)) {
                printf("[Friends][Game] ESC/Q pressed, exiting\n");
                running = false;
                continue;
            }
            if (event.type == SDL_EVENT_KEY_DOWN &&
                (event.key.key == SDLK_F11 ||
                 (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)))) {
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(window, fullscreen);
                SDL_GL_MakeCurrent(window, gl_context);
                printf("[Friends][Game] fullscreen %s\n", fullscreen ? "on" : "off");
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
                    if (g_nativeResize) g_nativeResize(fake_env, thiz, screen_width, screen_height);
                }
                continue;
            }
            ab_input_handle(&event);
        }

        /* Forward this frame's input. nativeInput keeps Classic's
         * (action, x, y, pointerId) order; nativeKeyInput here is 3 ints. */
        AbInputEvent ievs[64];
        int nev = ab_input_poll(ievs, 64);
        for (int i = 0; i < nev; i++) {
            const AbInputEvent* ie = &ievs[i];
            if (ie->type == AB_EV_POINTER) {
                if (g_nativeInput)
                    g_nativeInput(fake_env, thiz, ie->action, ie->x, ie->y, ie->pointer_id);
            } else if (ie->type == AB_EV_KEY && g_nativeKeyInput) {
                g_nativeKeyInput(fake_env, thiz, ie->keycode, ie->action, ie->unicode);
            }
        }

        audio_poll();

        present_begin();
        g_nativeUpdate(fake_env, thiz);   /* void: update and render together */
        present_end();

        if (shot_path && (long)frame_count == shot_frame) present_capture(shot_path);

        SDL_GL_SwapWindow(window);

        frame_count++;
        if (frame_count <= 20 || frame_count % 60 == 0) {
            printf("[Friends][Game] frame %lu\n", (unsigned long)frame_count);
            fflush(stdout);
        }

        if (autoclose && (long)frame_count >= autoclose && running) {
            printf("[Friends][Game] AB_AUTOCLOSE: closing after %ld frames\n", (long)frame_count);
            fflush(stdout);
            SDL_Event quit;
            SDL_zero(quit);
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }

        /* Friends is a 2013 build with no frame pacing of its own; a flat 16 ms
         * keeps the first bring-up runs from spinning a core. */
        SDL_Delay(16);
    }

    printf("[Friends][Game] exiting (frames: %lu, audio mixes: %lu)\n",
           (unsigned long)frame_count, audio_mix_calls());
    fflush(stdout);

    ab_input_shutdown();
    audio_shutdown();

    if (g_nativePause)  g_nativePause(fake_env, thiz);
    if (g_nativeDeinit) g_nativeDeinit(fake_env, thiz);

    printf("[Friends] terminated. reached stage %d (%s)\n", g_stage, stage_name(g_stage));
    fflush(stdout);
    fflush(stderr);
    /* Same reasoning as Classic: the engine keeps worker threads and has
     * finished writing its save, so hand the process back to the kernel rather
     * than racing SDL/EGL teardown against them. */
    _exit(0);
}
