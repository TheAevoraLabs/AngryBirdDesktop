/* game_config.c -- tiny "key value" settings file, no dependencies.
 *
 * The file lives next to the save data (./save/config.txt) and is created with
 * the defaults on first run, so everything is tunable without a rebuild or an
 * environment variable:
 *
 *   # window / presentation
 *   width 0               # 0 or "desktop" => the monitor's resolution
 *   height 0
 *   fullscreen 0          # 1 = start borderless fullscreen (F11 toggles)
 *   vsync 1
 *   scaler native         # native | linear | integer
 *   renderScale 100       # 100 = render at window size; 50 = render at half and upscale
 *
 *   # in-game cheats (applied to the save before the engine boots)
 *   powerups off          # off | max | 1..9999
 *   money off             # off | max | 1..9999999   (coins/gems/tickets)
 *   iap 1                 # fake Google Play billing: purchases always succeed
 *
 *   # language
 *   language auto         # auto | en | de | pt_BR | ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "game_config.h"
#include "util.h"

GameConfig config;
int screen_width = 1280;
int screen_height = 720;

void config_load_defaults(void) {
    memset(&config, 0, sizeof(config));
    strncpy(config.language, "auto", sizeof(config.language) - 1);
    strncpy(config.powerups, "off", sizeof(config.powerups) - 1);
    strncpy(config.money, "off", sizeof(config.money) - 1);
    /* 0 == "use the monitor's resolution": the engine scales its whole UI from
     * the size it is handed, so booting at the panel's native size looks
     * sharper for free, and the presentation scaler can still render below it. */
    config.width = 0;
    config.height = 0;
    config.fullscreen = 0;
    config.vsync = 1;
    strncpy(config.scaler, "linear", sizeof(config.scaler) - 1);
    config.render_scale = 100;
    config.iap = 1;
}

static void trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

/* value -> int, tolerant of "1280x720" style strings via `take_first` */
static int parse_int(const char *v, int fallback) {
    if (!v || !*v) return fallback;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return fallback;
    return (int)n;
}

/* A window dimension: a number, or a word meaning "ask the monitor" (0). */
static int parse_dim(const char *v, int fallback) {
    if (v && (!strcasecmp(v, "desktop") || !strcasecmp(v, "native") ||
              !strcasecmp(v, "auto") || !strcasecmp(v, "screen") ||
              !strcasecmp(v, "max")))
        return 0;
    return parse_int(v, fallback);
}

/* Accept "1280x720", "1280 x 720", "1280 720". */
static int parse_size(const char *v, int *w, int *h) {
    if (!v || !*v) return 0;
    char *end = NULL;
    long a = strtol(v, &end, 10);
    if (end == v) return 0;
    while (*end && (isspace((unsigned char)*end) || *end == 'x' || *end == 'X' || *end == ',')) end++;
    long b = strtol(end, &end, 10);
    if (*end != 0) return 0;
    if (a > 0) *w = (int)a;
    if (b > 0) *h = (int)b;
    return 1;
}

void config_load(const char *path) {
    /* Start from the defaults, then overlay the file. A file written by an
     * older build simply lacks the newer keys, and reading a fresh config struct
     * directly would leave those at zero ("width 0", "iap 0" -- silently wrong). */
    config_load_defaults();

    FILE *f = fopen(path, "rb");
    if (!f) {
        config_save(path);
        debugPrintf("[Config] created %s with defaults\n", path);
        return;
    }

    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (eq) *eq = ' ';
        trim(line);
        if (!line[0]) continue;

        char *sp = line;
        while (*sp && !isspace((unsigned char)*sp)) sp++;
        if (!*sp) continue;                 /* key with no value: ignore */
        *sp++ = 0;
        trim(sp);

        const char *key = line, *val = sp;
        if      (!strcasecmp(key, "language"))   snprintf(config.language, sizeof(config.language), "%s", val);
        else if (!strcasecmp(key, "powerups"))   snprintf(config.powerups, sizeof(config.powerups), "%s", val);
        else if (!strcasecmp(key, "money"))      snprintf(config.money, sizeof(config.money), "%s", val);
        else if (!strcasecmp(key, "iap") || !strcasecmp(key, "billing")) {
            config.iap = (!strcasecmp(val, "off") || !strcasecmp(val, "no") ||
                          !strcasecmp(val, "false") || !strcmp(val, "0")) ? 0 : 1;
        }
        else if (!strcasecmp(key, "fullscreen")) config.fullscreen = parse_int(val, 0) ? 1 : 0;
        else if (!strcasecmp(key, "vsync"))      config.vsync = parse_int(val, 1) ? 1 : 0;
        else if (!strcasecmp(key, "scaler"))     snprintf(config.scaler, sizeof(config.scaler), "%s", val);
        else if (!strcasecmp(key, "renderscale") ||
                 !strcasecmp(key, "render_scale")) config.render_scale = parse_int(val, 100);
        else if (!strcasecmp(key, "width"))      config.width = parse_dim(val, config.width);
        else if (!strcasecmp(key, "height"))     config.height = parse_dim(val, config.height);
        else if (!strcasecmp(key, "screen") || !strcasecmp(key, "resolution")) {
            if (!parse_size(val, &config.width, &config.height)) {
                config.width = parse_dim(val, config.width);
                config.height = config.width;
            }
        }
        else debugPrintf("[Config] %s:%d: unknown key '%s' (ignored)\n", path, lineno, key);
    }
    fclose(f);

    /* 0 is meaningful here ("monitor resolution"); anything else sane-sized is
     * clamped so a typo cannot ask for a 0-pixel drawable. */
    if (config.width != 0 && config.width < 320) config.width = 320;
    if (config.height != 0 && config.height < 240) config.height = 240;
    /* A dimension of 0 means "monitor resolution" -- and a monitor is not 0
     * tall, so if either side asks for it, both do. */
    if (config.width == 0 || config.height == 0) {
        config.width = 0;
        config.height = 0;
    }
    if (config.render_scale < 10) config.render_scale = 10;
    if (config.render_scale > 400) config.render_scale = 400;
}

int config_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f,
        "# AngryBirdsDesktop settings. '#' starts a comment; 'key value' or 'key=value'.\n"
        "\n"
        "# ---- window / presentation ----\n"
        "# width/height: window size in pixels. 0 (or 'desktop') = monitor resolution\n"
        "# ('screen 1920x1080' also works, and F11/Alt+Enter toggles fullscreen)\n"
        "width %d\n"
        "height %d\n"
        "# fullscreen: 1 = start borderless fullscreen. F11 / Alt+Enter toggles at runtime\n"
        "fullscreen %d\n"
        "vsync %d\n"
        "# scaler: native (engine draws straight to the window)\n"
        "#         linear (upscale a lower internal resolution, smooth)\n"
        "#         integer (upscale by whole numbers, crisp pixels)\n"
        "scaler %s\n"
        "# renderScale: internal render size as a %% of the window (100 = full size).\n"
        "# Below 100 the frame is rendered smaller and upscaled -- useful on weak GPUs.\n"
        "renderScale %d\n"
        "\n"
        "# ---- in-game extras (applied to the save before the engine boots) ----\n"
        "# powerups: off | max | 1..9999\n"
        "powerups %s\n"
        "# money: off | max | 1..9999999  (coins, gems, tickets)\n"
        "money %s\n"
        "# iap: 1 = fake Google Play billing (every purchase/restore succeeds)\n"
        "iap %d\n"
        "\n"
        "# ---- language ----\n"
        "# auto follows the host locale; otherwise a code like en, de, pt_BR, zh_TW\n"
        "language %s\n",
        config.width, config.height, config.fullscreen, config.vsync,
        config.scaler, config.render_scale, config.powerups, config.money,
        config.iap, config.language);
    fclose(f);
    return 0;
}
