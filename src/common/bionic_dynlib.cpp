/* bionic_dynlib.c -- Bionic/Android import table shared by every game driver.
 *
 * See bionic_dynlib.h for why this is not per-game.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#include <stdio.h>

#include "bionic_dynlib.h"
#include "android/asset_manager.h"
#include "android/log.h"
#include "android/stdio_compat.h"

/* Resolved from bionic_shims.c. */
extern "C" {
    extern const short *_tolower_tab_;
    extern const short *_toupper_tab_;
    extern const char  *_ctype_;
    /* __sF is declared by stdio_compat.h with Bionic's 84-byte element size;
     * the host's FILE would give the array the wrong stride. */
    extern void *memalign(size_t alignment, size_t size);
    extern size_t malloc_usable_size(void *ptr);
    extern ssize_t __read_chk(int fd, void *buf, size_t count, size_t buflen);

    /* Bionic's LP32 `struct sigaction` is 16 bytes (its sigset_t is one word),
     * glibc's is 140. The engine allocates the Bionic-sized struct on its stack
     * and hands the address to sigaction(), so calling glibc's directly walks
     * ~124 bytes past the end of that stack slot. These wrappers convert the
     * layouts; they are deliberately not named `sigaction`/`sigprocmask` so the
     * host libraries keep calling the real ones. */
    struct bionic_sigaction;
    extern int rovio_sigaction_compat(int signum, const struct bionic_sigaction *act,
                                      struct bionic_sigaction *oldact);
    extern int rovio_sigprocmask_compat(int how, const unsigned long *set,
                                        unsigned long *oldset);
}

static const so_default_dynlib g_bionic_dynlib[] = {
    { "__android_log_print", (uintptr_t)__android_log_print },
    { "__android_log_write", (uintptr_t)__android_log_write },
    { "AAssetManager_fromJava", (uintptr_t)AAssetManager_fromJava },
    { "AAssetManager_open", (uintptr_t)AAssetManager_open },
    { "AAsset_close", (uintptr_t)AAsset_close },
    { "AAsset_read", (uintptr_t)AAsset_read },
    { "AAsset_seek", (uintptr_t)AAsset_seek },
    { "AAsset_seek64", (uintptr_t)AAsset_seek64 },
    { "AAsset_getLength", (uintptr_t)AAsset_getLength },
    { "AAsset_getLength64", (uintptr_t)AAsset_getLength64 },
    { "AAsset_getRemainingLength", (uintptr_t)AAsset_getRemainingLength },
    { "AAsset_getRemainingLength64", (uintptr_t)AAsset_getRemainingLength64 },
    { "AAsset_getBuffer", (uintptr_t)AAsset_getBuffer },
    { "AAsset_isAllocated", (uintptr_t)AAsset_isAllocated },
    { "AAsset_openFileDescriptor", (uintptr_t)AAsset_openFileDescriptor },
    { "AAsset_openFileDescriptor64", (uintptr_t)AAsset_openFileDescriptor64 },
    { "AAssetManager_openDir", (uintptr_t)AAssetManager_openDir },
    { "AAssetDir_getNextFileName", (uintptr_t)AAssetDir_getNextFileName },
    { "AAssetDir_rewind", (uintptr_t)AAssetDir_rewind },
    { "AAssetDir_close", (uintptr_t)AAssetDir_close },
    { "_ctype_", (uintptr_t)&_ctype_ },
    { "_tolower_tab_", (uintptr_t)&_tolower_tab_ },
    { "_toupper_tab_", (uintptr_t)&_toupper_tab_ },
    { "__sF", (uintptr_t)&__sF },

    /* stdio: the engine was linked against Bionic, so its stdout/stderr are
     * `&__sF[1]` / `&__sF[2]` rather than the host's stream objects. Handing
     * those tokens straight to glibc faults inside the first fwrite(), so every
     * entry point that takes a FILE* goes through stdio_compat.c, which swaps a
     * token for the real host stream. Functions that merely *return* a FILE*
     * (fopen, fdopen, tmpfile) need no wrapper -- their result is already a
     * genuine host stream and passes through untouched. */
    { "fwrite",    (uintptr_t)rovio_fwrite_compat },
    { "fread",     (uintptr_t)rovio_fread_compat },
    { "fprintf",   (uintptr_t)rovio_fprintf_compat },
    { "vfprintf",  (uintptr_t)rovio_vfprintf_compat },
    { "fputs",     (uintptr_t)rovio_fputs_compat },
    { "fputc",     (uintptr_t)rovio_fputc_compat },
    { "putc",      (uintptr_t)rovio_putc_compat },
    { "getc",      (uintptr_t)rovio_getc_compat },
    { "ungetc",    (uintptr_t)rovio_ungetc_compat },
    { "fgets",     (uintptr_t)rovio_fgets_compat },
    { "fflush",    (uintptr_t)rovio_fflush_compat },
    { "fclose",    (uintptr_t)rovio_fclose_compat },
    { "feof",      (uintptr_t)rovio_feof_compat },
    { "ferror",    (uintptr_t)rovio_ferror_compat },
    { "fseek",     (uintptr_t)rovio_fseek_compat },
    { "fseeko",    (uintptr_t)rovio_fseeko_compat },
    { "ftell",     (uintptr_t)rovio_ftell_compat },
    { "ftello",    (uintptr_t)rovio_ftello_compat },
    { "setvbuf",   (uintptr_t)rovio_setvbuf_compat },
    { "freopen",   (uintptr_t)rovio_freopen_compat },
    { "getwc",     (uintptr_t)rovio_getwc_compat },
    { "putwc",     (uintptr_t)rovio_putwc_compat },
    { "ungetwc",   (uintptr_t)rovio_ungetwc_compat },

    { "memalign", (uintptr_t)memalign },
    { "malloc_usable_size", (uintptr_t)malloc_usable_size },
    { "__read_chk", (uintptr_t)__read_chk },
    { "sigaction", (uintptr_t)rovio_sigaction_compat },
    { "sigprocmask", (uintptr_t)rovio_sigprocmask_compat },
};

const so_default_dynlib *ab_bionic_dynlib(size_t *count) {
    if (count) *count = sizeof(g_bionic_dynlib) / sizeof(g_bionic_dynlib[0]);
    return g_bionic_dynlib;
}
