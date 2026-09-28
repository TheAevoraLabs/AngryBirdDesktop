#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>

#include "so_util.h"

#define ALIGN_PAGE_DOWN(x) ((x) & ~(4096 - 1))
#define ALIGN_PAGE_UP(x)   (((x) + 4096 - 1) & ~(4096 - 1))

int so_load(so_module *mod, const char *filename) {
    if (!mod || !filename) return -1;
    memset(mod, 0, sizeof(*mod));

    FILE *f = fopen(filename, "rb");
    if (!f) {
        printf("[ELF Loader] Failed to open %s: %s\n", filename, strerror(errno));
        return -1;
    }

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *file_buf = (uint8_t *)malloc(file_size);
    if (!file_buf) {
        fclose(f);
        return -1;
    }

    if (fread(file_buf, 1, file_size, f) != file_size) {
        printf("[ELF Loader] Failed to read complete file\n");
        free(file_buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_buf;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        printf("[ELF Loader] Invalid ELF magic\n");
        free(file_buf);
        return -1;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32 || ehdr->e_machine != EM_386) {
        printf("[ELF Loader] Only 32-bit x86 ELF is supported (class=%d, machine=%d)\n",
               ehdr->e_ident[EI_CLASS], ehdr->e_machine);
        free(file_buf);
        return -1;
    }

    // Find min and max virtual addresses across all PT_LOAD segments
    uintptr_t min_vaddr = (uintptr_t)-1;
    uintptr_t max_vaddr = 0;

    Elf32_Phdr *phdrs = (Elf32_Phdr *)(file_buf + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            if (phdrs[i].p_vaddr < min_vaddr) min_vaddr = phdrs[i].p_vaddr;
            if (phdrs[i].p_vaddr + phdrs[i].p_memsz > max_vaddr) {
                max_vaddr = phdrs[i].p_vaddr + phdrs[i].p_memsz;
            }
        }
    }

    if (min_vaddr == (uintptr_t)-1) {
        printf("[ELF Loader] No PT_LOAD segments found\n");
        free(file_buf);
        return -1;
    }

    min_vaddr = ALIGN_PAGE_DOWN(min_vaddr);
    max_vaddr = ALIGN_PAGE_UP(max_vaddr);
    size_t total_size = max_vaddr - min_vaddr;

    // Allocate memory with RWX permissions
    uint8_t *load_base = (uint8_t *)mmap(
        NULL, total_size,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0
    );

    if (load_base == MAP_FAILED) {
        printf("[ELF Loader] mmap failed for size 0x%zx: %s\n", total_size, strerror(errno));
        free(file_buf);
        return -1;
    }

    mod->base = load_base;
    mod->size = total_size;
    printf("[ELF Loader] Module base mapped at %p (size: 0x%zx)\n", mod->base, mod->size);

    // Copy PT_LOAD segments into memory
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            uint8_t *seg_dest = load_base + (phdrs[i].p_vaddr - min_vaddr);
            if (phdrs[i].p_filesz > 0) {
                memcpy(seg_dest, file_buf + phdrs[i].p_offset, phdrs[i].p_filesz);
            }
            if (phdrs[i].p_memsz > phdrs[i].p_filesz) {
                memset(seg_dest + phdrs[i].p_filesz, 0, phdrs[i].p_memsz - phdrs[i].p_filesz);
            }
        }
    }

    // Locate PT_DYNAMIC segment
    Elf32_Dyn *dyn = NULL;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn = (Elf32_Dyn *)(load_base + (phdrs[i].p_vaddr - min_vaddr));
            break;
        }
    }

    if (dyn) {
        for (; dyn->d_tag != DT_NULL; dyn++) {
            switch (dyn->d_tag) {
                case DT_SYMTAB:
                    mod->dynsym = (Elf32_Sym *)(load_base + (dyn->d_un.d_ptr - min_vaddr));
                    break;
                case DT_STRTAB:
                    mod->dynstr = (const char *)(load_base + (dyn->d_un.d_ptr - min_vaddr));
                    break;
                case DT_REL:
                    mod->rel = (Elf32_Rel *)(load_base + (dyn->d_un.d_ptr - min_vaddr));
                    break;
                case DT_RELSZ:
                    mod->num_rel = dyn->d_un.d_val / sizeof(Elf32_Rel);
                    break;
                case DT_JMPREL:
                    mod->plt_rel = (Elf32_Rel *)(load_base + (dyn->d_un.d_ptr - min_vaddr));
                    break;
                case DT_PLTRELSZ:
                    mod->num_plt_rel = dyn->d_un.d_val / sizeof(Elf32_Rel);
                    break;
                case DT_INIT:
                    mod->init_func = (void (*)(void))(load_base + (dyn->d_un.d_ptr - min_vaddr));
                    break;
                case DT_INIT_ARRAY:
                    mod->init_array = (void (**)(void))(load_base + (dyn->d_un.d_ptr - min_vaddr));
                    break;
                case DT_INIT_ARRAYSZ:
                    mod->init_array_size = dyn->d_un.d_val;
                    break;
                case DT_HASH:
                    mod->hash = (uint32_t *)(load_base + (dyn->d_un.d_ptr - min_vaddr));
                    break;
                case DT_GNU_HASH:
                    mod->gnu_hash = (uint32_t *)(load_base + (dyn->d_un.d_ptr - min_vaddr));
                    break;
            }
        }
    }

    // Determine number of dynamic symbols from DT_HASH or DT_GNU_HASH
    if (mod->hash) {
        mod->num_dynsym = mod->hash[1]; // nchain
    }

    // Parse section headers to find .eh_frame and .eh_frame_hdr
    if (ehdr->e_shoff != 0 && ehdr->e_shnum > 0) {
        Elf32_Shdr *shdrs = (Elf32_Shdr *)(file_buf + ehdr->e_shoff);
        if (ehdr->e_shstrndx < ehdr->e_shnum) {
            const char *shstrtab = (const char *)(file_buf + shdrs[ehdr->e_shstrndx].sh_offset);
            for (int i = 0; i < ehdr->e_shnum; i++) {
                const char *sname = shstrtab + shdrs[i].sh_name;
                if (strcmp(sname, ".eh_frame") == 0) {
                    mod->eh_frame = (void *)(mod->base + shdrs[i].sh_addr);
                } else if (strcmp(sname, ".eh_frame_hdr") == 0) {
                    mod->eh_frame_hdr = (void *)(mod->base + shdrs[i].sh_addr);
                }
            }
        }
    }

    printf("[ELF Loader] Loaded %s @ %p - %p (size: 0x%zx, eh_frame: %p)\n",
           filename, mod->base, mod->base + mod->size, mod->size, mod->eh_frame);

    free(file_buf);
    return 0;
}

int so_relocate(so_module *mod) {
    if (!mod || !mod->base) return -1;

    // Process REL table (R_386_RELATIVE and others)
    if (mod->rel && mod->num_rel > 0) {
        for (size_t i = 0; i < mod->num_rel; i++) {
            Elf32_Rel *rel = &mod->rel[i];
            uint32_t type = ELF32_R_TYPE(rel->r_info);
            uint32_t *target = (uint32_t *)(mod->base + rel->r_offset);

            if (type == R_386_RELATIVE) {
                *target += (uint32_t)mod->base;
            }
        }
    }

    // Process PLT REL table (R_386_JMP_SLOT, etc.)
    if (mod->plt_rel && mod->num_plt_rel > 0) {
        for (size_t i = 0; i < mod->num_plt_rel; i++) {
            Elf32_Rel *rel = &mod->plt_rel[i];
            uint32_t type = ELF32_R_TYPE(rel->r_info);
            uint32_t *target = (uint32_t *)(mod->base + rel->r_offset);

            if (type == R_386_RELATIVE) {
                *target += (uint32_t)mod->base;
            }
        }
    }

    return 0;
}

static uintptr_t find_symbol_in_table(const char *name, const so_default_dynlib *dynlib, int count) {
    if (!name || !dynlib) return 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(dynlib[i].symbol, name) == 0) {
            return dynlib[i].func;
        }
    }
    return 0;
}

static void process_relocs(so_module *mod, Elf32_Rel *rels, size_t count, const so_default_dynlib *default_dynlib, int size_default_dynlib) {
    for (size_t i = 0; i < count; i++) {
        Elf32_Rel *rel = &rels[i];
        uint32_t type = ELF32_R_TYPE(rel->r_info);
        uint32_t sym_idx = ELF32_R_SYM(rel->r_info);
        Elf32_Sym *sym = &mod->dynsym[sym_idx];
        uint32_t *target = (uint32_t *)(mod->base + rel->r_offset);

        if (type == R_386_GLOB_DAT || type == R_386_JMP_SLOT || type == R_386_32) {
            const char *name = mod->dynstr + sym->st_name;
            if (!name || name[0] == '\0') continue;

            // Strip version suffixes (e.g., name@GLIBC_...)
            char clean_name[256];
            strncpy(clean_name, name, sizeof(clean_name) - 1);
            clean_name[sizeof(clean_name) - 1] = '\0';
            char *at = strchr(clean_name, '@');
            if (at) *at = '\0';

            uintptr_t addr = 0;
            if (sym->st_shndx != SHN_UNDEF) {
                addr = (uintptr_t)(mod->base + sym->st_value);
            } else {
                // Check default dynlib table first
                addr = find_symbol_in_table(clean_name, default_dynlib, size_default_dynlib);
                if (!addr) {
                    // Fallback to dlsym from host libraries
                    addr = (uintptr_t)dlsym(RTLD_DEFAULT, clean_name);
                }
            }

            if (addr != 0) {
                if (type == R_386_32) {
                    *target += (uint32_t)addr;
                } else {
                    *target = (uint32_t)addr;
                }
            }
        }
    }
}

int so_resolve(so_module *mod, const so_default_dynlib *default_dynlib, int size_default_dynlib) {
    if (!mod || !mod->base || !mod->dynsym || !mod->dynstr) return -1;

    if (mod->rel && mod->num_rel > 0) {
        process_relocs(mod, mod->rel, mod->num_rel, default_dynlib, size_default_dynlib);
    }
    if (mod->plt_rel && mod->num_plt_rel > 0) {
        process_relocs(mod, mod->plt_rel, mod->num_plt_rel, default_dynlib, size_default_dynlib);
    }

    return 0;
}

void so_initialize(so_module *mod) {
    if (!mod || !mod->base) return;

    if (mod->eh_frame) {
        void (*so_register_frame)(const void *) = (void (*)(const void *))so_symbol(mod, "__register_frame");
        if (so_register_frame) {
            printf("[ELF Loader] Registering .eh_frame @ %p with SO unwinder...\n", mod->eh_frame);
            so_register_frame(mod->eh_frame);
        }
        void (*host_reg_frame)(const void *) = (void (*)(const void *))dlsym(RTLD_DEFAULT, "__register_frame");
        if (host_reg_frame) {
            printf("[ELF Loader] Registering .eh_frame @ %p with host unwinder...\n", mod->eh_frame);
            host_reg_frame(mod->eh_frame);
        }
    }

    if (mod->init_func) {
        printf("[ELF Loader] Calling DT_INIT @ %p...\n", mod->init_func);
        mod->init_func();
    }

    if (mod->init_array && mod->init_array_size > 0) {
        size_t count = mod->init_array_size / sizeof(uintptr_t);
        printf("[ELF Loader] Running initializers from DT_INIT_ARRAY (%zu slots)...\n", count);
        /* Two things make a naive walk unsafe here.
         *
         * First, the entries are not all constructors. Android toolchains mark
         * padding with (void*)-1 and glibc uses NULL, and where that marker sits
         * is not fixed: libAngryBirdsFriends.so has a -1 in slot 0 (with 52 real
         * constructors after it), while libAngryBirdsClassic.so starts straight
         * in on real code. So a marker is skipped, never treated as a stop.
         *
         * Second, calling whatever happens to be in the slot is how the Friends
         * bring-up used to jump to 0xffffffff. Only addresses that land inside
         * the module's own mapping are called; anything else is reported and
         * skipped, which keeps a malformed table from becoming a fault. */
        const uintptr_t lo = (uintptr_t)mod->base;
        const uintptr_t hi = lo + mod->size;
        size_t ran = 0, skipped = 0;
        for (size_t i = 0; i < count; i++) {
            uintptr_t addr = (uintptr_t)mod->init_array[i];
            if (addr == 0 || addr == (uintptr_t)-1) {
                skipped++;
                continue;
            }
            if (addr < lo || addr >= hi) {
                printf("[ELF Loader]   init_array[%zu] = %p outside module; skipping\n",
                       i, (void *)addr);
                skipped++;
                continue;
            }
            printf("[ELF Loader]   init_array[%zu] = %p\n", i, (void *)addr);
            fflush(stdout);
            ((void (*)(void))addr)();
            ran++;
        }
        printf("[ELF Loader] Ran %zu initializer(s), skipped %zu padding/foreign slot(s).\n",
               ran, skipped);
        fflush(stdout);
    }
    printf("[ELF Loader] Initializers completed successfully.\n");
}

uintptr_t so_symbol(so_module *mod, const char *symbol) {
    if (!mod || !mod->base || !mod->dynsym || !mod->dynstr || !symbol) return 0;

    for (size_t i = 0; i < mod->num_dynsym; i++) {
        Elf32_Sym *sym = &mod->dynsym[i];
        const char *name = mod->dynstr + sym->st_name;
        if (name && strcmp(name, symbol) == 0) {
            return (uintptr_t)(mod->base + sym->st_value);
        }
    }
    return 0;
}

void so_free(so_module *mod) {
    if (mod && mod->base && mod->size > 0) {
        munmap(mod->base, mod->size);
        memset(mod, 0, sizeof(*mod));
    }
}

void hook_addr_x86(uintptr_t addr, uintptr_t dst) {
    if (!addr || !dst) return;
    uintptr_t page = ALIGN_PAGE_DOWN(addr);
    mprotect((void *)page, 4096 * 2, PROT_READ | PROT_WRITE | PROT_EXEC);

    uint8_t *code = (uint8_t *)addr;
    int32_t rel = (int32_t)(dst - (addr + 5));

    code[0] = 0xE9; // JMP rel32
    *(int32_t *)(code + 1) = rel;
}
