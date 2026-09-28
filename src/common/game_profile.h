/* game_profile.h -- per-game description of everything the host needs to boot
 *                   one Rovio Fusion title.
 *
 * The host (loader, bionic shims, JNI bridge, audio, input, presentation) is
 * game-agnostic; what differs between titles is the *shape* of the engine's
 * Java-facing shell: which shared library to map, which symbols exist on it,
 * where its assets live and which writable directory it owns.
 *
 * Fusion changed that shell between generations. Angry Birds Classic 8.0.3
 * exposes com.rovio.fusion.NativeApplication (nativeConfig + nativeInit +
 * nativeRender + MyInputHandler), while Angry Birds Friends 1.0.0 (2013) hangs
 * everything off com.rovio.fusion.MyRenderer, has no separate config/render
 * step, and hands its writable directory to nativeInit instead.
 *
 * A profile therefore carries:
 *   - the library/asset/save locations for the title,
 *   - every engine entry point by name (an absent symbol is simply NULL), and
 *   - the internal functions we hook, resolved by exported name when the build
 *     has a symbol table and by module-relative offset when it does not.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#ifndef GAME_PROFILE_H
#define GAME_PROFILE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AB_GAME_CLASSIC = 0,
    AB_GAME_FRIENDS = 1,
    AB_GAME_COUNT
} ab_game_id;

/* Max entries in the path candidate lists (kept fixed so a profile stays a
 * plain static initialiser). */
#define AB_MAX_SO_CANDIDATES    8
#define AB_MAX_ASSET_CANDIDATES 4

/* An engine entry point. `symbol` is the exported JNI name; `offset` is a
 * module-relative fallback used only when the symbol is not exported. */
typedef struct {
    const char *symbol;
    uintptr_t   offset;
} ab_entry;

/* An internal engine function we want to hook. Resolution order is: module
 * offset (if non-zero) first, then the exported symbol name. Classic pins every
 * target by offset because that build is stripped; Friends exports a real
 * dynamic symbol table, so it can be resolved by name. */
typedef struct {
    const char *symbol;
    uintptr_t   offset;
} ab_hook_target;

typedef struct {
    ab_game_id  id;
    const char *name;         /* CLI / config key, e.g. "friends"       */
    const char *display_name; /* window title                           */

    const char *so_candidates[AB_MAX_SO_CANDIDATES];       /* first hit wins */
    const char *asset_candidates[AB_MAX_ASSET_CANDIDATES]; /* first hit wins */

    const char *save_dir;    /* writable root, e.g. "./save_friends" */
    const char *layout_dir;  /* <save_dir>/cache equivalent, may equal save_dir */
    const char *config_name; /* settings file, e.g. "./save_friends/config.txt" */
    /* UTF-8 string handed to the engine as its writable-data path. */
    const char *data_path;

    /* ---- engine entry points (symbol "" or NULL => the title lacks it) ---- */
    ab_entry jni_on_load;
    ab_entry config;      /* pre-init path setter (Classic only)            */
    ab_entry init;
    ab_entry deinit;
    ab_entry pause;
    ab_entry resume;
    ab_entry resize;
    ab_entry update;
    ab_entry render;      /* separate render pass (Classic only)            */
    ab_entry input;
    ab_entry key_input;
    ab_entry input_axis;  /* analog axes (Classic only)                     */
    ab_entry mix_data;    /* PCM pull mixer (both titles)                   */
    ab_entry get_orientations;
    ab_entry load_from_url;

    /* ---- internal hook targets ---- */
    ab_hook_target hk_lua_pcall;
    ab_hook_target hk_lua_tolstring;
    ab_hook_target hk_lua_load;
    ab_hook_target hk_luaG_runerror;
    ab_hook_target hk_is_drawing_ready;
    ab_hook_target hk_image_reader_create;
    ab_hook_target hk_detect_format;

    /* ---- behavioural differences that the driver loop must know about ---- */
    /* 1 = nativeUpdate returns jboolean and false means "engine is done"
     * 0 = nativeUpdate is void (Friends); never treat the result as a signal */
    int update_returns_bool;
    /* key input arity: Classic 4 ints (keyCode, action, unicode, deviceId),
     * Friends 3 ints (keyCode, action, unicode) */
    int key_input_args;
    /* Classic forces a VFS scheme value to 5 with an in-memory stub; Friends
     * needs no such patch (its assets sit at a different root). */
    int has_vfs_bundle_routing;
    /* 1 = this title's save files are the Classic RCS/settings.lua pair */
    int has_classic_save_patches;
} ab_game_profile;

const ab_game_profile *ab_game_profile_get(ab_game_id id);
/* Look up by name: "classic", "friends" (case-insensitive, also accepts
 * "abf"/"abc" and the display names). Returns NULL when unknown. */
const ab_game_profile *ab_game_profile_by_name(const char *name);

/* Resolve the requested game from the command line.
 * Recognises: --friends, --classic, --game <name>, --game=<name>.
 * Anything else is ignored. Returns `fallback` when nothing was requested, and
 * records the matched argv index (or -1) in *matched_index. */
ab_game_id ab_game_select(int argc, char **argv, ab_game_id fallback,
                          int *matched_index);

/* Resolve a hook target to an address inside `base`, or 0 when it is not
 * available. `mod` is the so_module* (opaque here to keep this header light). */
uintptr_t ab_hook_resolve(const ab_hook_target *t, void *mod, uint8_t *base);

#ifdef __cplusplus
}
#endif

#endif /* GAME_PROFILE_H */
