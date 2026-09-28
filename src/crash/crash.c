/* crash.c -- see crash.h.
 *
 * Everything the signal handler does is async-signal-safe: it formats into a
 * fixed static buffer with hand-rolled number printing (no printf, no malloc),
 * writes it with write(2), then forks and re-execs this binary to show the
 * dialog. SDL is never touched from the handler.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <execinfo.h>

#include <SDL3/SDL.h>

#include "crash.h"
#include "common/game_config.h"

#define REPORT_MAX 32768

static char      g_exe[512];
static char      g_dir[512];
static char      g_mod_name[64];
static uintptr_t g_mod_base;
static size_t    g_mod_size;
static volatile sig_atomic_t g_in_handler;
static char      g_report[REPORT_MAX];

/* --------------------------------------------------------------- formatting */

typedef struct { char *p; char *end; } Out;

static void o_ch(Out *o, char c) { if (o->p < o->end - 1) *o->p++ = c; }

static void o_str(Out *o, const char *s) { while (s && *s) o_ch(o, *s++); }

static void o_dec(Out *o, long long v) {
    char tmp[24];
    int n = 0;
    unsigned long long u = v < 0 ? (unsigned long long)(-v) : (unsigned long long)v;
    if (v < 0) o_ch(o, '-');
    do { tmp[n++] = (char)('0' + (u % 10)); u /= 10; } while (u);
    while (n) o_ch(o, tmp[--n]);
}

static void o_hex(Out *o, uintptr_t v) {
    static const char d[] = "0123456789abcdef";
    char tmp[2 * sizeof(uintptr_t) + 1];
    int n = 0;
    if (!v) { o_str(o, "0"); return; }
    while (v && n < (int)sizeof(tmp) - 1) { tmp[n++] = d[v & 0xf]; v >>= 4; }
    while (n) o_ch(o, tmp[--n]);
}

static void o_end(Out *o) { if (o->p < o->end) *o->p = 0; }

static const char *signame(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        case SIGTRAP: return "SIGTRAP";
        default:      return "signal";
    }
}

static const char *sigdesc(int sig) {
    switch (sig) {
        case SIGSEGV: return "invalid memory access";
        case SIGBUS:  return "bad memory access";
        case SIGILL:  return "illegal instruction";
        case SIGFPE:  return "arithmetic fault";
        case SIGABRT: return "abort (assertion, stack check or explicit abort)";
        case SIGTRAP: return "breakpoint/trap";
        default:      return "";
    }
}

/* ------------------------------------------------------------------ report */

static int build_report(char *dst, size_t cap, int sig, siginfo_t *info, void **frames, int nframes) {
    Out o = { dst, dst + cap };
    o_str(&o, "AngryBirdsDesktop crash report\n");
    o_str(&o, "==============================\n\n");
    o_str(&o, "signal : "); o_str(&o, signame(sig)); o_str(&o, " (");
    o_dec(&o, sig); o_str(&o, ") -- "); o_str(&o, sigdesc(sig)); o_ch(&o, '\n');
    if (info) {
        o_str(&o, "code   : "); o_dec(&o, info->si_code);
        if (info->si_code <= 0) {
            o_str(&o, " (SI_USER/TKILL from sender pid ");
            o_dec(&o, (long long)info->si_pid);
            o_str(&o, " uid ");
            o_dec(&o, (long long)info->si_uid);
            o_str(&o, ")\n");
        } else {
            o_str(&o, "\naddress: 0x"); o_hex(&o, (uintptr_t)info->si_addr); o_ch(&o, '\n');
        }
    }
    o_str(&o, "pid    : "); o_dec(&o, (long long)getpid());
    o_str(&o, "  thread: "); o_dec(&o, (long long)(unsigned)gettid()); o_ch(&o, '\n');
    o_str(&o, "time   : "); o_dec(&o, (long long)time(NULL)); o_ch(&o, '\n');
    if (g_mod_base) {
        o_str(&o, "module : "); o_str(&o, g_mod_name[0] ? g_mod_name : "game");
        o_str(&o, " @ "); o_hex(&o, g_mod_base);
        o_str(&o, " size 0x"); o_hex(&o, g_mod_size); o_ch(&o, '\n');
    }
    o_str(&o, "\nbacktrace (innermost first):\n");
    for (int i = 0; i < nframes; i++) {
        uintptr_t a = (uintptr_t)frames[i];
        o_str(&o, "  ["); o_dec(&o, i); o_str(&o, "] 0x"); o_hex(&o, a);
        if (g_mod_base && a >= g_mod_base && a < g_mod_base + g_mod_size) {
            o_str(&o, "  "); o_str(&o, g_mod_name[0] ? g_mod_name : "game");
            o_str(&o, "+0x"); o_hex(&o, a - g_mod_base);
        }
        o_ch(&o, '\n');
    }
    o_str(&o, "\nNote: offsets into the game module can be looked up with\n"
              "  objdump -d bin/libAngryBirdsClassic.so | less\n");
    o_end(&o);
    return (int)(o.p - dst);
}

/* --------------------------------------------------------------- the handler */

static void crash_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)ucontext;
    if (g_in_handler) _exit(128 + sig);   /* second fault while reporting */
    g_in_handler = 1;

    void *frames[48];
    int nframes = backtrace(frames, 48);

    int n = build_report(g_report, sizeof(g_report), sig, info, frames, nframes);

    char path[700];
    Out o = { path, path + sizeof(path) };
    o_str(&o, g_dir[0] ? g_dir : ".");
    o_str(&o, "/crash_");
    o_dec(&o, (long long)getpid());
    o_str(&o, ".log");
    o_end(&o);

    const char *pathp = path;
    int fd = open(pathp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(fd, g_report + off, (size_t)(n - off));
            if (w <= 0) break;
            off += w;
        }
        close(fd);
    }

    /* Also leave the report on stderr; a terminal run should not need the GUI. */
    write(2, "\n", 1);
    write(2, g_report, (size_t)n);

    /* Hand the dialog to a fresh copy of ourselves so a corrupted heap/GL state
     * cannot take the window down with it. Set AB_NO_CRASH_GUI=1 to skip. */
    if (!getenv("AB_NO_CRASH_GUI")) {
        pid_t pid = fork();
        if (pid == 0) {
            const char *exe = g_exe[0] ? g_exe : "/proc/self/exe";
            char *argv[4];
            argv[0] = (char *)exe;
            argv[1] = (char *)"--crash-report";
            argv[2] = (char *)pathp;
            argv[3] = NULL;
            setenv("AB_CRASH_CHILD", "1", 1);
            execv(exe, argv);
            _exit(127);
        }
    }

    /* Die for real: default disposition, same signal, so the exit status and any
     * core dump describe what actually happened. */
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

void crash_init(const char *exe_path, const char *report_dir) {
    if (exe_path) snprintf(g_exe, sizeof(g_exe), "%s", exe_path);
    if (report_dir) snprintf(g_dir, sizeof(g_dir), "%s", report_dir);
    else snprintf(g_dir, sizeof(g_dir), ".");

    /* /proc/self/exe is the reliable one for a re-exec. */
    char link[512];
    ssize_t n = readlink("/proc/self/exe", link, sizeof(link) - 1);
    if (n > 0) { link[n] = 0; snprintf(g_exe, sizeof(g_exe), "%s", link); }

    /* Warm up the unwinder: backtrace() allocates on its first call, which must
     * not happen inside the handler. */
    void *warm[8];
    backtrace(warm, 8);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    /* SIGSTKSZ is not a compile-time constant on modern glibc, so pick our own
     * (64 KiB is plenty for backtrace()). */
    static char altstack[64 * 1024];
    stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);

    /* A broken pipe on stdout must not kill us mid-frame. */
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "[Crash] handler installed (SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT/SIGTRAP)\n");
}

void crash_set_module(const char *name, uintptr_t base, size_t size) {
    snprintf(g_mod_name, sizeof(g_mod_name), "%s", name ? name : "");
    g_mod_base = base;
    g_mod_size = size;
}

void crash_test_trigger(void) {
    fprintf(stderr, "[Crash] --crash-test: raising SIGSEGV on purpose\n");
    fflush(stderr);
    raise(SIGSEGV);
    /* the handler should never return; if it did, make it obvious */
    abort();
}

/* ------------------------------------------------------------------ the GUI */

int crash_gui_mode(int argc, char **argv, const char *report_path_out, size_t out_len) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--crash-report") || !strcmp(argv[i], "-crash-report")) {
            if (i + 1 < argc) {
                snprintf((char *)report_path_out, out_len, "%s", argv[i + 1]);
                return 1;
            }
            return 1;
        }
    }
    return 0;
}

static void read_text(const char *path, char *dst, size_t cap) {
    dst[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(dst, cap, "(could not open %s: %s)", path, strerror(errno)); return; }
    size_t n = fread(dst, 1, cap - 1, f);
    dst[n] = 0;
    fclose(f);
}

int crash_show_dialog(const char *report_path) {
    static char text[26000];
    read_text(report_path, text, sizeof(text));

    /* First line of the report makes a decent window title. */
    char title[256] = "Angry Birds Desktop - crashed";
    char *nl = strchr(text, '\n');
    if (nl) {
        *nl = 0;
        snprintf(title, sizeof(title), "Angry Birds Desktop - %s", text);
        *nl = '\n';
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[Crash] SDL_Init failed (%s); report is at %s\n", SDL_GetError(), report_path);
        return 1;
    }

    /* SDL draws this natively (GTK/portal on Linux) -- no font or toolkit of our
     * own, which is the whole point. */
    SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR, title,
        text[0] ? text : "No report contents.",
        NULL);

    char note[512];
    snprintf(note, sizeof(note),
             "Report saved to:\n%s\n\nThe game has stopped. Check the log above "
             "and restart when you are ready.", report_path);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Crash report saved", note, NULL);

    SDL_Quit();
    return 0;
}
