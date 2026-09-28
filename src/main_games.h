/* main_games.h -- one driver per supported Fusion title.
 *
 * Each driver owns its own window/context/loop because the engine lifecycles
 * genuinely differ (Classic: config -> init -> resize -> resume -> update +
 * render; Friends: init(w, h, dataPath) -> resize -> resume -> update, where
 * update renders too). They share everything below that: the ELF loader, the
 * Bionic shims, the JNI bridge, audio, input, presentation and the crash
 * handler.
 *
 * Both drivers are entered through main_dispatch.cpp, which picks one from the
 * command line.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#ifndef MAIN_GAMES_H
#define MAIN_GAMES_H

#ifdef __cplusplus
extern "C" {
#endif

/* Angry Birds Classic 8.0.3 (src/main.cpp). */
int ab_run_classic(int argc, char **argv);

/* Angry Birds Friends 1.0.0 (src/main_friends.cpp). */
int ab_run_friends(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_GAMES_H */
