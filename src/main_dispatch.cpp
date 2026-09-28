/* main_dispatch.cpp -- process entry point; picks a game driver.
 *
 * Usage:
 *   angrybirds_desktop                  # Angry Birds Classic (default)
 *   angrybirds_desktop --friends        # Angry Birds Friends
 *   angrybirds_desktop --game friends   # same, spelled out
 *   angrybirds_desktop --classic        # explicit default
 *   angrybirds_desktop --list-games     # show the known profiles
 *
 * The crash dialog also lives here: the signal handler re-executes this binary
 * as `angrybirds_desktop --crash-report <file>`, and that has to be handled
 * before any game driver starts touching SDL or the loader.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>

#include "main_games.h"
#include "common/game_profile.h"
#include "crash/crash.h"

static void print_usage(const char *argv0) {
    printf("AngryBirdsDesktop -- in-memory x86 Android Fusion host\n"
           "\n"
           "usage: %s [options]\n"
           "\n"
           "  --classic            boot Angry Birds Classic 8.0.3 (default)\n"
           "  --friends            boot Angry Birds Friends 1.0.0\n"
           "  --game <name>        explicitly select a profile\n"
           "  --game=<name>        same as above\n"
           "  --list-games         list the known game profiles\n"
           "  -h, --help           this message\n"
           "\n"
           "Per-game settings live in save/config.txt (Classic) and\n"
           "save_friends/config.txt (Friends); assets are resolved from each\n"
           "profile's own directory so the titles cannot collide.\n",
           argv0 ? argv0 : "angrybirds_desktop");
}

static void print_games(void) {
    printf("known game profiles:\n");
    for (int i = 0; i < AB_GAME_COUNT; i++) {
        const ab_game_profile *p = ab_game_profile_get((ab_game_id)i);
        if (!p) continue;
        printf("  %-8s %s\n", p->name, p->display_name);
        printf("           lib:    %s\n", p->so_candidates[0] ? p->so_candidates[0] : "(none)");
        printf("           assets: %s\n", p->asset_candidates[0] ? p->asset_candidates[0] : "(none)");
        printf("           save:   %s\n", p->save_dir ? p->save_dir : "(none)");
    }
}

int main(int argc, char **argv) {
    /* The crash handler forks and re-execs us with this flag; a healthy process
     * then draws the report window. Must come first -- nothing else is safe to
     * touch on that path. */
    {
        char report[600] = {0};
        if (crash_gui_mode(argc, argv, report, sizeof(report))) {
            if (!report[0]) {
                fprintf(stderr, "usage: %s --crash-report <file>\n",
                        argv[0] ? argv[0] : "angrybirds_desktop");
                return 2;
            }
            return crash_show_dialog(report);
        }
    }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a) continue;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            print_usage(argv[0]);
            return 0;
        }
        if (!strcmp(a, "--list-games")) {
            print_games();
            return 0;
        }
    }

    int matched = -1;
    ab_game_id id = ab_game_select(argc, argv, AB_GAME_CLASSIC, &matched);
    const ab_game_profile *prof = ab_game_profile_get(id);
    if (!prof) {
        fprintf(stderr, "[Profile] no profile for game id %d\n", (int)id);
        return 2;
    }

    printf("[Profile] selected game: %s (%s)%s\n",
           prof->name, prof->display_name,
           matched >= 0 ? "" : "  [default]");
    fflush(stdout);

    if (id == AB_GAME_FRIENDS)
        return ab_run_friends(argc, argv);

    return ab_run_classic(argc, argv);
}
