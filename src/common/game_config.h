#ifndef GAME_CONFIG_H
#define GAME_CONFIG_H

#define SO_NAME  "bin/libAngryBirdsClassic.so"
#define DATA_DIR "./save"
#define LOG_NAME "./run.log"
#define CONFIG_NAME "./save/config.txt"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* engine/UI language: "auto" follows the host locale */
    char language[16];
    /* power-up top-up applied to settings.lua before boot: off | max | 1..9999 */
    char powerups[32];
    /* wallet top-up applied to the RCS inventory cache: off | max | 1..9999999 */
    char money[32];
    /* fake Google Play billing: purchases and restores always succeed */
    int  iap;
    /* presentation */
    int  width, height;      /* window size; 0 => 1280x720 */
    int  fullscreen;         /* start borderless-fullscreen */
    int  vsync;
    char scaler[16];         /* native | linear | integer */
    int  render_scale;       /* 100 = render at window size; 50 = half, etc. */
} GameConfig;

extern GameConfig config;
extern int screen_width;
extern int screen_height;

void config_load_defaults(void);
/* Read key/value lines from `path` (created with the defaults if missing).
 * Unknown keys are reported once and otherwise ignored. */
void config_load(const char *path);
/* Write the current settings out (used to create the file on first run). */
int  config_save(const char *path);

#ifdef __cplusplus
}
#endif

#endif // GAME_CONFIG_H
