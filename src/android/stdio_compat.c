/* stdio_compat.c -- see stdio_compat.h for why this exists.
 *
 * Short version: the engine was linked against Bionic, where the standard
 * streams live in the array `FILE __sF[3]` and `stdout`/`stderr` are macros for
 * `&__sF[1]` / `&__sF[2]`. Those pointers are meaningless to the host's glibc,
 * so every FILE*-taking stdio call is intercepted and the three stream tokens
 * are swapped for the host's real streams.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>

#include "stdio_compat.h"

FILE *rovio_stream_compat(FILE *stream) {
    /* Offsets are computed explicitly rather than by indexing the array: the
     * whole point is that the stride belongs to Bionic (84 bytes), not to the
     * host's FILE (148). See the note in stdio_compat.h. */
    const uintptr_t base = (uintptr_t)&__sF[0];
    const uintptr_t p = (uintptr_t)stream;

    if (p == base)                             return stdin;
    if (p == base + AB_BIONIC_FILE_SIZE)       return stdout;
    if (p == base + 2 * AB_BIONIC_FILE_SIZE)   return stderr;

    /* Landed inside the array but not on a slot boundary: the engine is doing
     * arithmetic on __sF that we did not anticipate. Say so, because the
     * alternative is forwarding that pointer to glibc and dying inside
     * fwrite() with no clue which stream was meant. */
    if (p > base && p < base + 3 * AB_BIONIC_FILE_SIZE) {
        static int warned = 0;
        if (warned < 8) {
            warned++;
            fprintf(stderr,
                    "[stdio] unrecognised __sF pointer %p (base %p, +%lu) forward unchanged\n",
                    (void *)p, (void *)base, (unsigned long)(p - base));
        }
    }

    /* Not one of ours: a FILE* from fopen/fdopen/tmpfile, or NULL. */
    return stream;
}

int rovio_fwrite_compat(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return (int)fwrite(ptr, size, nmemb, rovio_stream_compat(stream));
}

size_t rovio_fread_compat(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fread(ptr, size, nmemb, rovio_stream_compat(stream));
}

int rovio_fprintf_compat(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = vfprintf(rovio_stream_compat(stream), format, ap);
    va_end(ap);
    return r;
}

int rovio_vfprintf_compat(FILE *stream, const char *format, va_list ap) {
    return vfprintf(rovio_stream_compat(stream), format, ap);
}

int rovio_fputs_compat(const char *s, FILE *stream) {
    return fputs(s, rovio_stream_compat(stream));
}

int rovio_fputc_compat(int c, FILE *stream) {
    return fputc(c, rovio_stream_compat(stream));
}

int rovio_putc_compat(int c, FILE *stream) {
    return fputc(c, rovio_stream_compat(stream));
}

int rovio_getc_compat(FILE *stream) {
    return fgetc(rovio_stream_compat(stream));
}

int rovio_ungetc_compat(int c, FILE *stream) {
    return ungetc(c, rovio_stream_compat(stream));
}

char *rovio_fgets_compat(char *s, int size, FILE *stream) {
    return fgets(s, size, rovio_stream_compat(stream));
}

int rovio_fflush_compat(FILE *stream) {
    /* NULL passes straight through: fflush(NULL) flushes every stream. */
    return fflush(rovio_stream_compat(stream));
}

/* Bionic would let the engine close stdout/stderr, and so would we if this were
 * a faithful shim -- but closing the host's console streams breaks every
 * subsequent diagnostic the loader itself prints (and this host has no console
 * to reopen). Flush and report success instead; the process's own streams stay
 * usable. A FILE* that came from fopen is closed normally. */
int rovio_fclose_compat(FILE *stream) {
    /* Compare the *resolved* stream: that keeps this correct no matter how the
     * engine spelled the __sF token it handed us. */
    FILE *host = rovio_stream_compat(stream);
    if (host == stdin || host == stdout || host == stderr) {
        return fflush(host);
    }
    return fclose(stream);
}

int rovio_feof_compat(FILE *stream) {
    return feof(rovio_stream_compat(stream));
}

int rovio_ferror_compat(FILE *stream) {
    return ferror(rovio_stream_compat(stream));
}

/* Offsets are `long` on purpose: the engine is a 32-bit Bionic build, so an
 * off_t reaches us as one 32-bit word even if this host's fseeko() is the
 * 64-bit variant. Reading a long (never an off_t) keeps the cdecl arguments
 * aligned with what the engine actually pushed. */
int rovio_fseek_compat(FILE *stream, long offset, int whence) {
    return fseeko(rovio_stream_compat(stream), (off_t)offset, whence);
}

int rovio_fseeko_compat(FILE *stream, long offset, int whence) {
    return fseeko(rovio_stream_compat(stream), (off_t)offset, whence);
}

long rovio_ftell_compat(FILE *stream) {
    return (long)ftello(rovio_stream_compat(stream));
}

long rovio_ftello_compat(FILE *stream) {
    return (long)ftello(rovio_stream_compat(stream));
}

int rovio_setvbuf_compat(FILE *stream, char *buf, int mode, size_t size) {
    return setvbuf(rovio_stream_compat(stream), buf, mode, size);
}

/* freopen()'s stream argument is one of the three tokens just as often as it is
 * a real file handle; its return value is always a genuine host FILE*. */
FILE *rovio_freopen_compat(const char *pathname, const char *mode, FILE *stream) {
    return freopen(pathname, mode, rovio_stream_compat(stream));
}

/* Both engines import getwc/putwc/ungetwc, so they reach us with a __sF slot as
 * the FILE* exactly like fprintf does. The bodies call the f-prefixed forms,
 * which are real functions even where the short names are macros. */
wint_t rovio_getwc_compat(FILE *stream) {
    return fgetwc(rovio_stream_compat(stream));
}

wint_t rovio_putwc_compat(wchar_t wc, FILE *stream) {
    return fputwc(wc, rovio_stream_compat(stream));
}

wint_t rovio_ungetwc_compat(wint_t wc, FILE *stream) {
    return ungetwc(wc, rovio_stream_compat(stream));
}
