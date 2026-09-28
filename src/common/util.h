#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>

#define debugPrintf printf
#define debugLogFlush() fflush(stdout)

static inline uint64_t umin(uint64_t a, uint64_t b) {
    return (a < b) ? a : b;
}

#endif // UTIL_H
