/* crash.h -- in-process crash handler + native crash dialog.
 *
 * When the loader, a shim, or the game itself takes a fatal signal we want three
 * things: a readable report on disk, a window the user can actually see, and the
 * process still dying the normal way (so the exit code and any core dump are
 * honest).
 *
 * The handler itself only uses async-signal-safe primitives (open/write/fork/
 * execv). It writes the report, then re-executes *this* binary as
 * `angrybirds_desktop --crash-report <file>`, which shows the dialog in a clean
 * process, and finally re-raises the signal with the default disposition.
 */

#ifndef AB_CRASH_H
#define AB_CRASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install handlers for SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT/SIGTRAP.
 * `exe_path` is used for the re-exec (the client passes argv[0]; /proc/self/exe
 * is preferred when available), `report_dir` is where crash_*.log is written. */
void crash_init(const char *exe_path, const char *report_dir);

/* Remember the loaded game module so report frames can be shown as
 * "<name>+0xOFFSET" instead of a raw address. */
void crash_set_module(const char *name, uintptr_t base, size_t size);

/* True when this process was started as the crash dialog. */
int  crash_gui_mode(int argc, char **argv, const char *report_path_out, size_t out_len);

/* Show the report window for `report_path`. Returns 0 if it was displayed. */
int  crash_show_dialog(const char *report_path);

/* Deliberately fault, for testing the pipeline (--crash-test). */
void crash_test_trigger(void);

#ifdef __cplusplus
}
#endif

#endif /* AB_CRASH_H */
