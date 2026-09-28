/* stdio_compat.h -- map Bionic's __sF streams onto the host's real streams.
 *
 * Bionic does not export `stdout`/`stderr`/`stdin` as variables the way glibc
 * does; instead the C library exposes a single array
 *
 *     FILE __sF[3];              // __sF[0]=stdin, [1]=stdout, [2]=stderr
 *
 * and <stdio.h> defines the familiar names as address macros, so
 * `fprintf(stderr, ...)` compiles to a call passing `&__sF[2]`. Both Angry
 * Birds builds import `__sF` plus the stdio calls, which means every stream
 * write in the engine arrives as a pointer into *our* array.
 *
 * Handing that pointer to the host's glibc is the bug: glibc treats it as an
 * `_IO_FILE*`, but it is not one -- it is whatever we put there. This module
 * therefore intercepts every stdio entry point that takes a `FILE*` and
 * substitutes the real host stream for a `__sF` slot before forwarding:
 *
 *     fprintf(&__sF[2], ...)  ->  fprintf(stderr, ...)
 *
 * A `FILE*` that did *not* come from `__sF` (i.e. one from fopen/fdopen/tmpfile)
 * is forwarded untouched, so ordinary file I/O is unaffected.
 *
 * Follows the same pattern as rovio_sigaction_compat() in bionic_shims.c: the
 * wrappers are deliberately not named `fwrite`, `fprintf`, ... so the host's own
 * libraries keep calling the real ones.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */

#ifndef AB_STDIO_COMPAT_H
#define AB_STDIO_COMPAT_H

#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bionic's `struct __sFILE` is 84 bytes on 32-bit; glibc's `FILE` is 148.
 *
 * That difference is the whole reason this file exists. The engine computes the
 * standard streams *itself*: Bionic's <stdio.h> defines `stderr` as `&__sF[2]`,
 * which with an 84-byte stride is `__sF + 168`. Declaring the array as the
 * host's `FILE[3]` gave it a 148-byte stride, so our `&__sF[2]` sat at +296
 * while the engine kept handing us +168 -- the token never matched, the raw
 * pointer was forwarded to glibc, and the first fwrite() faulted.
 *
 * (Measured against the running engine: the stream it passed to fwrite was
 * `__sF+168`, exactly 2 * 84.)
 *
 * So the array must carry Bionic's element size, not the host's. It holds no
 * real FILE state either way -- the slots are address tokens that
 * rovio_stream_compat() maps onto genuine host streams. */
#define AB_BIONIC_FILE_SIZE 84

typedef struct ab_bionic_file {
    unsigned char opaque[AB_BIONIC_FILE_SIZE];
} ab_bionic_file;

/* Defined in bionic_shims.c. */
extern ab_bionic_file __sF[3];

/* Resolve a stream the engine handed us to the host's real stream, or return it
 * unchanged when it is not one of the three __sF slots. NULL stays NULL (which
 * matters: fflush(NULL) means "flush every stream"). */
FILE *rovio_stream_compat(FILE *stream);

int    rovio_fwrite_compat(const void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t rovio_fread_compat(void *ptr, size_t size, size_t nmemb, FILE *stream);
int    rovio_fprintf_compat(FILE *stream, const char *format, ...);
int    rovio_vfprintf_compat(FILE *stream, const char *format, va_list ap);
int    rovio_fputs_compat(const char *s, FILE *stream);
int    rovio_fputc_compat(int c, FILE *stream);
int    rovio_putc_compat(int c, FILE *stream);
int    rovio_getc_compat(FILE *stream);
int    rovio_ungetc_compat(int c, FILE *stream);
char  *rovio_fgets_compat(char *s, int size, FILE *stream);
int    rovio_fflush_compat(FILE *stream);
int    rovio_fclose_compat(FILE *stream);
int    rovio_feof_compat(FILE *stream);
int    rovio_ferror_compat(FILE *stream);
int    rovio_fseek_compat(FILE *stream, long offset, int whence);
int    rovio_fseeko_compat(FILE *stream, long offset, int whence);
long   rovio_ftell_compat(FILE *stream);
long   rovio_ftello_compat(FILE *stream);
int    rovio_setvbuf_compat(FILE *stream, char *buf, int mode, size_t size);
FILE  *rovio_freopen_compat(const char *pathname, const char *mode, FILE *stream);

/* The wide-character family takes a FILE* too, and both engines import at least
 * getwc/putwc/ungetwc, so they need the same translation. */
wint_t rovio_getwc_compat(FILE *stream);
wint_t rovio_putwc_compat(wchar_t wc, FILE *stream);
wint_t rovio_ungetwc_compat(wint_t wc, FILE *stream);

#ifdef __cplusplus
}
#endif

#endif /* AB_STDIO_COMPAT_H */
