/* game_profile.c -- the two Fusion titles this host knows how to boot.
 *
 * Everything game-specific lives in the two tables at the bottom of this file.
 * Adding a third title should mean adding one table, not touching the loader.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "game_profile.h"
#include "loader/so_util.h"

/* ------------------------------------------------------------------ *
 * Angry Birds Classic 8.0.3 (Fusion 8.x)
 *
 * This build is stripped, so every hook target is pinned by module-relative
 * offset -- the same numbers main.cpp has always used. The offsets are NOT
 * portable to another build of the same game; they are facts about this exact
 * libAngryBirdsClassic.so (md5 50abc80e1612cf4377a3dbefba4d6966).
 * ------------------------------------------------------------------ */
static const ab_game_profile k_classic = {
    .id = AB_GAME_CLASSIC,
    .name = "classic",
    .display_name = "Angry Birds Classic",

    .so_candidates = {
        "bin/libAngryBirdsClassic.so",
        "angry-birds-classic-8-0-3/lib/x86/libAngryBirdsClassic.so",
        "./libAngryBirdsClassic.so",
        "../bin/libAngryBirdsClassic.so",
        NULL
    },
    .asset_candidates = {
        "assets",
        "../assets",
        NULL
    },

    .save_dir    = "./save",
    .layout_dir  = "./save/cache",
    .config_name = "./save/config.txt",
    .data_path   = "./save",

    .jni_on_load = { "JNI_OnLoad", 0 },
    .config      = { "Java_com_rovio_fusion_NativeApplication_nativeConfig", 0 },
    .init        = { "Java_com_rovio_fusion_NativeApplication_nativeInit", 0 },
    .deinit      = { "Java_com_rovio_fusion_NativeApplication_nativeDeinit", 0 },
    .pause       = { "Java_com_rovio_fusion_NativeApplication_nativePause", 0 },
    .resume      = { "Java_com_rovio_fusion_NativeApplication_nativeResume", 0 },
    .resize      = { "Java_com_rovio_fusion_NativeApplication_nativeResize", 0 },
    .update      = { "Java_com_rovio_fusion_NativeApplication_nativeUpdate", 0 },
    .render      = { "Java_com_rovio_fusion_NativeApplication_nativeRender", 0 },
    .input       = { "Java_com_rovio_fusion_MyInputHandler_nativeInput", 0 },
    .key_input   = { "Java_com_rovio_fusion_MyInputHandler_nativeKeyInput", 0 },
    .input_axis  = { "Java_com_rovio_fusion_MyInputHandler_nativeInputAxis", 0 },
    .mix_data    = { "Java_com_rovio_fusion_AudioOutput_nativeMixData", 0 },
    .get_orientations = { NULL, 0 },
    .load_from_url    = { NULL, 0 },

    .hk_lua_pcall            = { NULL, 0x8d66f0 },
    .hk_lua_tolstring        = { NULL, 0x8d49f0 },
    .hk_lua_load             = { NULL, 0x8d68d0 },
    .hk_luaG_runerror        = { NULL, 0x8c7160 },
    .hk_is_drawing_ready     = { NULL, 0x2176a0 },
    .hk_image_reader_create  = { NULL, 0x70fa00 },
    .hk_detect_format        = { NULL, 0x74b160 },

    .update_returns_bool     = 1,
    .key_input_args          = 4,
    .has_vfs_bundle_routing  = 1,
    .has_classic_save_patches = 1,
};

/* ------------------------------------------------------------------ *
 * Angry Birds Friends 1.0.0 (2013, Fusion ~3.x)
 *
 * Same engine family, older shell. Differences that matter:
 *   - there is no com.rovio.fusion.NativeApplication and no MyInputHandler;
 *     the whole surface hangs off com.rovio.fusion.MyRenderer (with a
 *     MyLegacyRenderer twin for the GLES1 path),
 *   - the writable-data path arrives as the *fifth* argument of nativeInit
 *     instead of through a separate nativeConfig call:
 *       nativeInit(JNIEnv*, jobject, int width, int height, jstring dataPath)
 *   - nativeUpdate(JNIEnv*, jobject) is void, so its return value is not a
 *     shutdown signal,
 *   - nativeKeyInput takes three ints, not four,
 *   - unlike Classic, this binary keeps a rich dynamic symbol table
 *     (13.5k symbols), so hook targets resolve by exported name.
 *
 * Assets deliberately resolve to a Friends-specific root so nothing can write
 * into, or shadow, the Classic tree at ./assets.
 * ------------------------------------------------------------------ */
static const ab_game_profile k_friends = {
    .id = AB_GAME_FRIENDS,
    .name = "friends",
    .display_name = "Angry Birds Friends",

    .so_candidates = {
        "bin/libAngryBirdsFriends.so",
        "angry-birds-friends-1-0-0/lib/x86/libAngryBirdsFriends.so",
        "./libAngryBirdsFriends.so",
        "../bin/libAngryBirdsFriends.so",
        NULL
    },
    /* A dedicated directory is preferred, so a user can drop their own copy in
     * without touching the pristine extraction. Falls back to the in-repo
     * extraction; never falls back to the Classic ./assets. */
    .asset_candidates = {
        "friends_assets",
        "angry-birds-friends-1-0-0/assets",
        "../angry-birds-friends-1-0-0/assets",
        NULL
    },

    .save_dir    = "./save_friends",
    .layout_dir  = "./save_friends/cache",
    .config_name = "./save_friends/config.txt",
    .data_path   = "./save_friends",

    .jni_on_load = { "JNI_OnLoad", 0 },
    .config      = { NULL, 0 },   /* Friends has no nativeConfig */
    .init        = { "Java_com_rovio_fusion_MyRenderer_nativeInit", 0 },
    .deinit      = { "Java_com_rovio_fusion_MyRenderer_nativeDeinit", 0 },
    .pause       = { "Java_com_rovio_fusion_MyRenderer_nativePause", 0 },
    .resume      = { "Java_com_rovio_fusion_MyRenderer_nativeResume", 0 },
    .resize      = { "Java_com_rovio_fusion_MyRenderer_nativeResize", 0 },
    .update      = { "Java_com_rovio_fusion_MyRenderer_nativeUpdate", 0 },
    .render      = { NULL, 0 },   /* rendering happens inside nativeUpdate */
    .input       = { "Java_com_rovio_fusion_MyRenderer_nativeInput", 0 },
    .key_input   = { "Java_com_rovio_fusion_MyRenderer_nativeKeyInput", 0 },
    .input_axis  = { NULL, 0 },
    .mix_data    = { "Java_com_rovio_fusion_AudioOutput_nativeMixData", 0 },
    .get_orientations = { "Java_com_rovio_fusion_MyRenderer_nativeGetPossibleOrientations", 0 },
    .load_from_url    = { "Java_com_rovio_fusion_MyRenderer_nativeLoadFromUrl", 0 },

    /* Exported as plain C symbols (nm -D shows "lua_pcall", "lua_load", ...).
     * The remaining Classic hooks have no exported counterpart in this build,
     * so they stay disabled until their offsets are derived. */
    .hk_lua_pcall            = { "lua_pcall", 0 },
    .hk_lua_tolstring        = { "lua_tolstring", 0 },
    .hk_lua_load             = { "lua_load", 0 },
    .hk_luaG_runerror        = { NULL, 0 },
    .hk_is_drawing_ready     = { NULL, 0 },
    .hk_image_reader_create  = { NULL, 0 },
    .hk_detect_format        = { NULL, 0 },

    .update_returns_bool     = 0,
    .key_input_args          = 3,
    .has_vfs_bundle_routing  = 0,
    .has_classic_save_patches = 0,
};

static const ab_game_profile *const k_profiles[AB_GAME_COUNT] = {
    [AB_GAME_CLASSIC] = &k_classic,
    [AB_GAME_FRIENDS] = &k_friends,
};

const ab_game_profile *ab_game_profile_get(ab_game_id id) {
    if (id < 0 || id >= AB_GAME_COUNT) return NULL;
    return k_profiles[id];
}

const ab_game_profile *ab_game_profile_by_name(const char *name) {
    if (!name || !*name) return NULL;
    if (!strcasecmp(name, "classic") || !strcasecmp(name, "abc") ||
        !strcasecmp(name, "angrybirds") || !strcasecmp(name, "angry-birds-classic"))
        return &k_classic;
    if (!strcasecmp(name, "friends") || !strcasecmp(name, "abf") ||
        !strcasecmp(name, "angrybirdsfriends") || !strcasecmp(name, "angry-birds-friends"))
        return &k_friends;
    return NULL;
}

ab_game_id ab_game_select(int argc, char **argv, ab_game_id fallback,
                          int *matched_index) {
    ab_game_id selected = fallback;
    int matched = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a) continue;

        const ab_game_profile *hit = NULL;

        /* --game=<name> */
        if (!strncmp(a, "--game=", 7)) {
            hit = ab_game_profile_by_name(a + 7);
            if (!hit) {
                fprintf(stderr, "[Profile] unknown game '%s' in %s\n", a + 7, a);
                continue;
            }
        /* --game <name> */
        } else if (!strcmp(a, "--game") && i + 1 < argc) {
            hit = ab_game_profile_by_name(argv[i + 1]);
            if (!hit) {
                fprintf(stderr, "[Profile] unknown game '%s' after --game\n", argv[i + 1]);
                continue;
            }
            i++;
        /* convenience switches */
        } else if (!strcmp(a, "--friends") || !strcmp(a, "-friends")) {
            hit = &k_friends;
        } else if (!strcmp(a, "--classic") || !strcmp(a, "-classic")) {
            hit = &k_classic;
        }

        if (hit) {
            selected = hit->id;
            matched = i;
        }
    }

    if (matched_index) *matched_index = matched;
    return selected;
}

uintptr_t ab_hook_resolve(const ab_hook_target *t, void *mod, uint8_t *base) {
    if (!t) return 0;
    /* Offset first: it is exact and does not depend on the symbol surviving in
     * the dynamic table. Name second: the friendlier option for builds that
     * still export their internals. */
    if (t->offset && base) return (uintptr_t)(base + t->offset);
    if (t->symbol && mod) return so_symbol((so_module *)mod, t->symbol);
    return 0;
}
