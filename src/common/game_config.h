#ifndef GAME_CONFIG_H
#define GAME_CONFIG_H

#define SO_NAME  "bin/libAngryBirdsClassic.so"
#define DATA_DIR "./save"
#define LOG_NAME "./run.log"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char language[16];
    char powerups[16];
} GameConfig;

extern GameConfig config;
extern int screen_width;
extern int screen_height;

void config_load_defaults(void);

#ifdef __cplusplus
}
#endif

#endif // GAME_CONFIG_H
