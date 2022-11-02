/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _LINKER_H_
#define _LINKER_H_

#include <unistd.h>
#include <sys/types.h>
#include <elf.h>
#include <gelf.h>
#include <link.h>
#include <stdbool.h>

#if defined(__aarch64__) || defined(__x86_64__)
#define USE_RELA
#endif

#undef PAGE_MASK
#undef PAGE_SIZE
#define PAGE_SIZE 4096
#define PAGE_MASK 4095

#ifndef R_AARCH64_TLS_DTPREL32
#define R_AARCH64_TLS_DTPREL32	1031
#pragma message "The R_AARCH64_TLS_DTPREL32 was not set :("
#endif

void apkenv_debugger_init();

/* magic shared structures that GDB knows about */

#if 0
struct link_map
{
    uintptr_t l_addr;
    char * l_name;
    uintptr_t l_ld;
    struct link_map * l_next;
    struct link_map * l_prev;
};

/* needed for dl_iterate_phdr to be passed to the callbacks provided */
struct dl_phdr_info
{
    GElf_Addr dlpi_addr;
    const char *dlpi_name;
    const GElf_Phdr *dlpi_phdr;
    GElf_Half dlpi_phnum;
};


// Values for r_debug->state
enum {
    RT_CONSISTENT,
    RT_ADD,
    RT_DELETE
};

struct r_debug
{
    int32_t r_version;
    struct link_map * r_map;
    void (*r_brk)(void);
    int32_t r_state;
    uintptr_t r_ldbase;
};
#endif

typedef struct soinfo soinfo;

#define FLAG_LINKED     0x00000001
#define FLAG_ERROR      0x00000002
#define FLAG_EXE        0x00000004 // The main executable
#define FLAG_LINKER     0x00000010 // The linker itself

#define SOINFO_NAME_LEN 128

struct soinfo
{
    const char name[SOINFO_NAME_LEN];
    GElf_Phdr *phdr;
    size_t phnum;
    GElf_Addr entry;
    GElf_Addr base;
    size_t size;

    uint32_t unused;  // DO NOT USE, maintained for compatibility.

    GElf_Dyn *dynamic;

    uint32_t wrprotect_start;
    uint32_t wrprotect_end;

    soinfo *next;
    uint32_t flags;

    const char *strtab;
    GElf_Sym *symtab;

    size_t nbucket;
    size_t nchain;
    uint32_t *bucket;
    uint32_t *chain;

    GElf_Addr **plt_got;

#if defined(USE_RELA)
	GElf_Rela *plt_rela;
	size_t plt_rela_count;
	GElf_Rela *rela;
	size_t rela_count;
#else
	GElf_Rel *plt_rel;
	size_t plt_rel_count;
	GElf_Rel *rel;
	size_t rel_count;
#endif

    intptr_t *preinit_array;
    size_t preinit_array_count;

    intptr_t *init_array;
    size_t init_array_count;
    intptr_t *fini_array;
    size_t fini_array_count;

    void (*init_func)(void);
    void (*fini_func)(void);

#if defined(__arm__)
  // ARM EABI section used for stack unwinding.
	uint32_t *ARM_exidx;
	size_t ARM_exidx_count;
#elif defined(__mips__)
	uint32_t mips_symtabno;
	uint32_t mips_local_gotno;
	uint32_t mips_gotsym;
#endif

    size_t refcount;
    struct link_map linkmap;

    bool constructors_called;

    GElf_Addr gnu_relro_start;
    unsigned gnu_relro_len;

    /* apkenv stuff */
    char fullpath[SOINFO_NAME_LEN];
};


extern soinfo apkenv_libdl_info;

#ifndef DT_INIT_ARRAY
#define DT_INIT_ARRAY      25
#endif

#ifndef DT_FINI_ARRAY
#define DT_FINI_ARRAY      26
#endif

#ifndef DT_INIT_ARRAYSZ
#define DT_INIT_ARRAYSZ    27
#endif

#ifndef DT_FINI_ARRAYSZ
#define DT_FINI_ARRAYSZ    28
#endif

#ifndef DT_PREINIT_ARRAY
#define DT_PREINIT_ARRAY   32
#endif

#ifndef DT_PREINIT_ARRAYSZ
#define DT_PREINIT_ARRAYSZ 33
#endif

soinfo *apkenv_find_library(const char *name, const bool try_glibc);
unsigned apkenv_unload_library(soinfo *si);
GElf_Sym *apkenv_lookup_in_library(soinfo *si, const char *name);
GElf_Sym *apkenv_lookup(const char *name, soinfo **found, soinfo *start);
soinfo *apkenv_find_containing_library(const void *addr);
GElf_Sym *apkenv_find_containing_symbol(const void *addr, soinfo *si);
const char *apkenv_linker_get_error(void);
void apkenv_call_constructors_recursive(soinfo *si);

#if defined(__arm__)
typedef long unsigned int *_Unwind_Ptr;
_Unwind_Ptr apkenv_dl_unwind_find_exidx(_Unwind_Ptr pc, int *pcount);
#elif defined(__aarch64__) || defined(__i386__) || defined(__mips__) || defined(__x86_64__)
int apkenv_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *);
#endif

void apkenv_notify_gdb_of_libraries(void);
int apkenv_add_sopath(const char *path);

bool do_we_have_this_handle(void *handle);

#endif
