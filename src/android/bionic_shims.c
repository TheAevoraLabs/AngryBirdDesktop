#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <ctype.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1. Memory Allocator Override (bypasses glibc 32-bit sbrk/top-chunk limits)
struct MmapHeader {
    void* raw_ptr;
    size_t raw_size;
    uint32_t magic;
};
#define MMAP_MAGIC 0xAB12CD34

void* memalign(size_t alignment, size_t size) {
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    if (size == 0) size = 1;
    
    // Allocate extra space for header + alignment padding
    size_t total = size + sizeof(struct MmapHeader) + alignment;
    char* raw = (char*)mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return NULL;
    
    uintptr_t p = (uintptr_t)(raw + sizeof(struct MmapHeader));
    uintptr_t aligned = (p + alignment - 1) & ~(alignment - 1);
    
    struct MmapHeader* h = (struct MmapHeader*)(aligned - sizeof(struct MmapHeader));
    h->raw_ptr = raw;
    h->raw_size = total;
    h->magic = MMAP_MAGIC;
    return (void*)aligned;
}

void* malloc(size_t size) {
    return memalign(16, size);
}

void free(void* ptr) {
    if (!ptr) return;
    struct MmapHeader* h = (struct MmapHeader*)(((char*)ptr) - sizeof(struct MmapHeader));
    if (h->magic == MMAP_MAGIC) {
        munmap(h->raw_ptr, h->raw_size);
    }
}

void* calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void* p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    struct MmapHeader* h = (struct MmapHeader*)(((char*)ptr) - sizeof(struct MmapHeader));
    if (h->magic != MMAP_MAGIC) {
        return NULL;
    }
    size_t old_user_size = h->raw_size - (size_t)((char*)ptr - (char*)h->raw_ptr);
    void* new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    
    size_t copy_len = (size < old_user_size) ? size : old_user_size;
    memcpy(new_ptr, ptr, copy_len);
    free(ptr);
    return new_ptr;
}

size_t malloc_usable_size(void* ptr) {
    if (!ptr) return 0;
    struct MmapHeader* h = (struct MmapHeader*)(((char*)ptr) - sizeof(struct MmapHeader));
    if (h->magic == MMAP_MAGIC) {
        return h->raw_size - (size_t)((char*)ptr - (char*)h->raw_ptr);
    }
    return 0;
}

int posix_memalign(void** memptr, size_t alignment, size_t size) {
    void* ptr = memalign(alignment, size);
    if (!ptr) return ENOMEM;
    *memptr = ptr;
    return 0;
}

void* aligned_alloc(size_t alignment, size_t size) {
    return memalign(alignment, size);
}

void* valloc(size_t size) {
    return memalign(4096, size);
}

void* pvalloc(size_t size) {
    size_t pagesize = 4096;
    size_t rounded_size = (size + pagesize - 1) & ~(pagesize - 1);
    return memalign(pagesize, rounded_size);
}

// 2. __errno
int* __errno(void) {
    return __errno_location();
}

// 3. __google_potentially_blocking_region
void __google_potentially_blocking_region_begin(void) {}
void __google_potentially_blocking_region_end(void) {}

// 4. Pure Linux Futex-based Bionic 4-byte Mutex & Condvar
#include <sys/syscall.h>
#include <linux/futex.h>

static inline int sys_futex(void *uaddr, int op, int val, const struct timespec *timeout, void *uaddr2, int val3) {
    return syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3);
}

static inline pid_t get_tid(void) {
    return (pid_t)syscall(SYS_gettid);
}

#define BIONIC_MUTEX_TYPE_MASK 0x03
#define BIONIC_MUTEX_TYPE_NORMAL 0
#define BIONIC_MUTEX_TYPE_RECURSIVE 1
#define BIONIC_MUTEX_TYPE_ERRORCHECK 2

#define BIONIC_MUTEX_OWNER_MASK 0xFFFF0000
#define BIONIC_MUTEX_COUNT_MASK 0x0000FFFC
#define BIONIC_MUTEX_COUNT_SHIFT 2

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
    if (!mutex) return EINVAL;
    int type = 0;
    if (attr) {
        type = *(const int*)attr & 3;
    }
    *(volatile int*)mutex = type;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t* mutex) {
    volatile int* val = (volatile int*)mutex;
    if (!val) return EINVAL;

    pid_t tid = get_tid() & 0xFFFF;
    int owner_bits = tid << 16;

    while (1) {
        int cur = *val;
        int owner = cur & BIONIC_MUTEX_OWNER_MASK;
        int type = cur & BIONIC_MUTEX_TYPE_MASK;

        if (owner == 0) {
            int desired = owner_bits | (1 << BIONIC_MUTEX_COUNT_SHIFT) | type;
            int expected = cur;
            if (__atomic_compare_exchange_n(val, &expected, desired, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                return 0;
            }
            continue;
        }

        if (owner == owner_bits) {
            int count = ((cur & BIONIC_MUTEX_COUNT_MASK) >> BIONIC_MUTEX_COUNT_SHIFT) + 1;
            *val = owner_bits | (count << BIONIC_MUTEX_COUNT_SHIFT) | type;
            return 0;
        }

        sys_futex((void*)val, FUTEX_WAIT_PRIVATE, cur, NULL, NULL, 0);
    }
}

int pthread_mutex_trylock(pthread_mutex_t* mutex) {
    volatile int* val = (volatile int*)mutex;
    if (!val) return EINVAL;

    pid_t tid = get_tid() & 0xFFFF;
    int owner_bits = tid << 16;

    int cur = *val;
    int owner = cur & BIONIC_MUTEX_OWNER_MASK;
    int type = cur & BIONIC_MUTEX_TYPE_MASK;

    if (owner == 0) {
        int desired = owner_bits | (1 << BIONIC_MUTEX_COUNT_SHIFT) | type;
        int expected = cur;
        if (__atomic_compare_exchange_n(val, &expected, desired, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return 0;
        }
        return EBUSY;
    }

    if (owner == owner_bits) {
        int count = ((cur & BIONIC_MUTEX_COUNT_MASK) >> BIONIC_MUTEX_COUNT_SHIFT) + 1;
        *val = owner_bits | (count << BIONIC_MUTEX_COUNT_SHIFT) | type;
        return 0;
    }

    return EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    volatile int* val = (volatile int*)mutex;
    if (!val) return EINVAL;

    pid_t tid = get_tid() & 0xFFFF;
    int owner_bits = tid << 16;

    int cur = *val;
    int owner = cur & BIONIC_MUTEX_OWNER_MASK;
    int type = cur & BIONIC_MUTEX_TYPE_MASK;

    if (owner != owner_bits) {
        return EPERM;
    }

    int count = ((cur & BIONIC_MUTEX_COUNT_MASK) >> BIONIC_MUTEX_COUNT_SHIFT) - 1;
    if (count > 0) {
        *val = owner_bits | (count << BIONIC_MUTEX_COUNT_SHIFT) | type;
        return 0;
    }

    *val = type;
    sys_futex((void*)val, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex) {
    if (mutex) *(volatile int*)mutex = 0;
    return 0;
}

int pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    (void)attr;
    if (cond) *(volatile int*)cond = 0;
    return 0;
}

int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    volatile int* cval = (volatile int*)cond;
    if (!cval || !mutex) return -1;
    int seq = *cval;
    pthread_mutex_unlock(mutex);
    sys_futex((void*)cval, FUTEX_WAIT_PRIVATE, seq, NULL, NULL, 0);
    pthread_mutex_lock(mutex);
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* abstime) {
    volatile int* cval = (volatile int*)cond;
    if (!cval || !mutex) return -1;
    int seq = *cval;
    pthread_mutex_unlock(mutex);
    
    struct timespec rel;
    if (abstime) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        rel.tv_sec = abstime->tv_sec - now.tv_sec;
        rel.tv_nsec = abstime->tv_nsec - now.tv_nsec;
        if (rel.tv_nsec < 0) {
            rel.tv_sec--;
            rel.tv_nsec += 1000000000L;
        }
        if (rel.tv_sec < 0) {
            rel.tv_sec = 0;
            rel.tv_nsec = 0;
        }
        sys_futex((void*)cval, FUTEX_WAIT_PRIVATE, seq, &rel, NULL, 0);
    } else {
        sys_futex((void*)cval, FUTEX_WAIT_PRIVATE, seq, NULL, NULL, 0);
    }
    
    pthread_mutex_lock(mutex);
    return 0;
}

int pthread_cond_timedwait_monotonic(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* abstime) {
    return pthread_cond_timedwait(cond, mutex, abstime);
}

int pthread_cond_signal(pthread_cond_t* cond) {
    volatile int* cval = (volatile int*)cond;
    if (!cval) return -1;
    __atomic_add_fetch(cval, 1, __ATOMIC_RELEASE);
    sys_futex((void*)cval, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t* cond) {
    volatile int* cval = (volatile int*)cond;
    if (!cval) return -1;
    __atomic_add_fetch(cval, 1, __ATOMIC_RELEASE);
    sys_futex((void*)cval, FUTEX_WAKE_PRIVATE, 2147483647, NULL, NULL, 0);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t* cond) {
    if (cond) *(volatile int*)cond = 0;
    return 0;
}

// 5. __system_property_get
int __system_property_get(const char* name, char* value) {
    if (!name || !value) return 0;
    if (strcmp(name, "ro.build.version.sdk") == 0) {
        strcpy(value, "28");
        return (int)strlen(value);
    }
    if (strcmp(name, "ro.build.version.release") == 0) {
        strcpy(value, "9.0");
        return (int)strlen(value);
    }
    if (strcmp(name, "ro.product.model") == 0) {
        strcpy(value, "Linux Desktop");
        return (int)strlen(value);
    }
    value[0] = '\0';
    return 0;
}

int fsync(int fd) {
    if (fd < 0) return 0;
    int ret = (int)syscall(SYS_fsync, fd);
    if (ret < 0 && errno == EBADF) return 0;
    return ret;
}

int fdatasync(int fd) {
    if (fd < 0) return 0;
    int ret = (int)syscall(SYS_fdatasync, fd);
    if (ret < 0 && errno == EBADF) return 0;
    return ret;
}

// 6. Ctype tables for Bionic
static short g_tolower_tab[384];
static short g_toupper_tab[384];
static char  g_ctype_tab[384];

const short* _tolower_tab_ = &g_tolower_tab[0];
const short* _toupper_tab_ = &g_toupper_tab[0];
const char*  _ctype_ = &g_ctype_tab[0];

// 7. __sF (Bionic stdin, stdout, stderr)
FILE __sF[3];

__attribute__((constructor))
static void init_bionic_tables(void) {
    // Initialize ctype tables
    g_tolower_tab[0] = -1;
    g_toupper_tab[0] = -1;
    g_ctype_tab[0] = 0;

    for (int i = 0; i < 256; i++) {
        g_tolower_tab[i + 1] = (short)tolower(i);
        g_toupper_tab[i + 1] = (short)toupper(i);
        
        char flags = 0;
        if (isupper(i)) flags |= 0x01; // _U
        if (islower(i)) flags |= 0x02; // _L
        if (isdigit(i)) flags |= 0x04; // _N
        if (isspace(i)) flags |= 0x08; // _S
        if (ispunct(i)) flags |= 0x10; // _P
        if (iscntrl(i)) flags |= 0x20; // _C
        if (isxdigit(i)) flags |= 0x40; // _X (Bionic: 0x40)
        if (isblank(i)) flags |= 0x80; // _B (Bionic: 0x80)
        g_ctype_tab[i + 1] = flags;
    }

    if (stdin)  memcpy(&__sF[0], stdin, sizeof(FILE));
    if (stdout) memcpy(&__sF[1], stdout, sizeof(FILE));
    if (stderr) memcpy(&__sF[2], stderr, sizeof(FILE));
}

#include <sys/syscall.h>

ssize_t read(int fd, void *buf, size_t count) {
    if (fd == 0) {
        return 0; // EOF on stdin
    }
    return syscall(SYS_read, fd, buf, count);
}

ssize_t __read_chk(int fd, void *buf, size_t count, size_t buflen) {
    (void)buflen;
    return read(fd, buf, count);
}

// ---------------------------------------------------------------------------
// 8. Bionic-ABI sigaction / sigprocmask compatibility wrappers
//
// libAngryBirdsClassic.so is compiled against Bionic headers, where
// `struct sigaction` is only 16 bytes on 32-bit x86:
//   handler(4) | sa_mask(4, unsigned long) | sa_flags(4) | sa_restorer(4)
//
// glibc's struct sigaction is 140 bytes (handler(4) | sa_mask(128) | ...).
// When the game calls glibc sigaction directly with a 16-byte buffer, glibc
// and the kernel read/write 140 bytes and smash the stack canary. These
// wrappers convert between the two layouts. They are named distinctly so they
// do NOT interpose the global `sigaction` symbol (SDL3 calls the real glibc
// sigaction with glibc-format structs); instead the game's own GOT slots for
// sigaction/sigprocmask are redirected here from main.cpp.
// ---------------------------------------------------------------------------

struct bionic_sigaction {
    void* sa_handler;      // union { sa_handler; sa_sigaction; }
    unsigned long sa_mask; // sigset_t = unsigned long on Bionic LP32
    int sa_flags;
    void* sa_restorer;
};

/* glibc i386 struct sigaction (only the fields we touch) */
struct glibc_sigaction {
    void* sa_handler;
    unsigned char sa_mask[128];
    int sa_flags;
    void* sa_restorer;
};

static int (*g_real_sigaction)(int, const struct glibc_sigaction*, struct glibc_sigaction*) = NULL;
static int (*g_real_sigprocmask)(int, const void*, void*) = NULL;

int rovio_sigaction_compat(int signum, const struct bionic_sigaction* act, struct bionic_sigaction* oldact) {
    if (!g_real_sigaction) {
        g_real_sigaction = (int (*)(int, const struct glibc_sigaction*, struct glibc_sigaction*))dlsym(RTLD_NEXT, "sigaction");
        if (!g_real_sigaction) {
            printf("[Bionic] ERROR: could not resolve real sigaction: %s\n", dlerror());
            return -1;
        }
    }

    struct glibc_sigaction g_act, g_old;
    memset(&g_act, 0, sizeof(g_act));
    memset(&g_old, 0, sizeof(g_old));

    if (act) {
        g_act.sa_handler = act->sa_handler;
        /* Bionic 4-byte mask covers signals 1..32, same bit layout as glibc's first word */
        memcpy(g_act.sa_mask, &act->sa_mask, sizeof(act->sa_mask));
        g_act.sa_flags = act->sa_flags;
        g_act.sa_restorer = act->sa_restorer;
    }

    int r = g_real_sigaction(signum, act ? &g_act : NULL, oldact ? &g_old : NULL);

    if (r == 0 && oldact) {
        oldact->sa_handler = g_old.sa_handler;
        oldact->sa_mask = 0;
        memcpy(&oldact->sa_mask, g_old.sa_mask, sizeof(oldact->sa_mask));
        oldact->sa_flags = g_old.sa_flags;
        oldact->sa_restorer = g_old.sa_restorer;
    }
    return r;
}

int rovio_sigprocmask_compat(int how, const unsigned long* set, unsigned long* oldset) {
    if (!g_real_sigprocmask) {
        g_real_sigprocmask = (int (*)(int, const void*, void*))dlsym(RTLD_NEXT, "sigprocmask");
        if (!g_real_sigprocmask) {
            printf("[Bionic] ERROR: could not resolve real sigprocmask: %s\n", dlerror());
            return -1;
        }
    }

    sigset_t g_set, g_old;
    memset(&g_set, 0, sizeof(g_set));
    memset(&g_old, 0, sizeof(g_old));

    if (set) memcpy(&g_set, set, sizeof(unsigned long));

    int r = g_real_sigprocmask(how, set ? &g_set : NULL, oldset ? &g_old : NULL);

    if (r == 0 && oldset) {
        *oldset = 0;
        memcpy(oldset, &g_old, sizeof(unsigned long));
    }
    return r;
}

#ifdef __cplusplus
}
#endif

