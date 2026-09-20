#include "log.h"
#include <stdio.h>
#include <time.h>
#include <stdint.h>

// Bionic x86 stack canary guard symbol
uintptr_t __stack_chk_guard = 0x595e9fbd;

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio;
    va_list ap;
    printf("[%s] ", tag ? tag : "Android");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    (void)prio;
    printf("[%s] ", tag ? tag : "Android");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    return 0;
}

int __android_log_write(int prio, const char *tag, const char *text) {
    (void)prio;
    printf("[%s] %s\n", tag ? tag : "Android", text ? text : "");
    fflush(stdout);
    return 0;
}
