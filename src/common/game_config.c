#include "game_config.h"
#include <string.h>

GameConfig config;
int screen_width = 1280;
int screen_height = 720;

void config_load_defaults(void) {
    strncpy(config.language, "en", sizeof(config.language) - 1);
    strncpy(config.powerups, "off", sizeof(config.powerups) - 1);
}
