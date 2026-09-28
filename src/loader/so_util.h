#ifndef SO_UTIL_H
#define SO_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <elf.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct so_default_dynlib {
    const char *symbol;
    uintptr_t func;
} so_default_dynlib;

typedef struct so_module {
    uint8_t *base;
    size_t size;

    Elf32_Sym *dynsym;
    const char *dynstr;
    size_t num_dynsym;

    Elf32_Rel *rel;
    size_t num_rel;

    Elf32_Rel *plt_rel;
    size_t num_plt_rel;

    uint32_t *hash;
    uint32_t *gnu_hash;

    void (**init_array)(void);
    size_t init_array_size;
    void (*init_func)(void);

    Elf32_Phdr *phdr;
    size_t phnum;

    void *eh_frame;
    void *eh_frame_hdr;
} so_module;

int so_load(so_module *mod, const char *filename);
int so_relocate(so_module *mod);
int so_resolve(so_module *mod, const so_default_dynlib *default_dynlib, int size_default_dynlib);
void so_initialize(so_module *mod);
uintptr_t so_symbol(so_module *mod, const char *symbol);
void so_free(so_module *mod);

// In-memory function detour/hook helper (5-byte JMP on x86)
void hook_addr_x86(uintptr_t addr, uintptr_t dst);

#ifdef __cplusplus
}
#endif

#endif // SO_UTIL_H
