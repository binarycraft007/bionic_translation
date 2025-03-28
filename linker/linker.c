/*
 * Copyright (C) 2008, 2009 The Android Open Source Project
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

#if LINKER_DEBUG
#define LINKER_DEBUG_PRINTF(...) PRINT(__VA_ARGS__)
#else
#define LINKER_DEBUG_PRINTF(...)
#endif

#include <linux/auxvec.h>

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fnmatch.h>

#include <pthread.h>

#include <sys/mman.h>
#include <sys/param.h>

#include <libgen.h>

/* eglGetProcAddress to import funny extensions that Android exports but Mesa sometimes doesn't */
#include <EGL/egl.h>

/* sigsetjmp may or may not be a macro; we can't wrap it, so we need to substitute it for the system version when linking */
#include <setjmp.h>

/* special private C library header - see Android.mk */
#include "bionic_tls.h"

#include "strlcpy.h"

#include "../wrapper/wrapper.h"

#include "config.h"
#include "linker.h"
#include "linker_debug.h"
#include "linker_environ.h"
#include "linker_format.h"

#define ALLOW_SYMBOLS_FROM_MAIN 1
#define SO_MAX 128

/* Assume average path length of 64 and max 8 paths */
#define LDPATH_BUFSIZE	  512
#define LDPATH_MAX	  8

#define LDPRELOAD_BUFSIZE 512
#define LDPRELOAD_MAX	  8

#ifdef __LP64__
#define ELF_R_SYM   ELF64_R_SYM
#define ELF_R_TYPE  ELF64_R_TYPE
#define ELF_ST_BIND ELF64_ST_BIND
#define ELF_ST_TYPE ELF64_ST_TYPE
#else
#define ELF_R_SYM   ELF32_R_SYM
#define ELF_R_TYPE  ELF32_R_TYPE
#define ELF_ST_BIND ELF32_ST_BIND
#define ELF_ST_TYPE ELF32_ST_TYPE
#endif

/* >>> IMPORTANT NOTE - READ ME BEFORE MODIFYING <<<
 *
 * Do NOT use malloc() and friends or pthread_*() code here.
 * Don't use printf() either; it's caused mysterious memory
 * corruption in the past.
 * The linker runs before we bring up libc and it's easiest
 * to make sure it does not depend on any complex libc features
 *
 * open issues / todo:
 *
 * - are we doing everything we should for ARM_COPY relocations?
 * - cleaner error reporting
 * - after linking, set as much stuff as possible to READONLY
 *   and NOEXEC
 * - linker hardcodes PAGE_SIZE and PAGE_MASK because the kernel
 *   headers provide versions that are negative...
 * - allocate space for soinfo structs dynamically instead of
 *   having a hard limit (64)
 */

static int apkenv_link_image(soinfo *si, unsigned wr_offset);

static int apkenv_socount = 0;
static soinfo apkenv_sopool[SO_MAX];
static soinfo *apkenv_freelist = NULL;
static soinfo *apkenv_solist = &apkenv_libdl_info;
static soinfo *apkenv_sonext = &apkenv_libdl_info;
#if ALLOW_SYMBOLS_FROM_MAIN
static soinfo *apkenv_somain; /* main process, always the one after apkenv_libdl_info */
#endif

bool do_we_have_this_handle(void *handle)
{
	soinfo *si;
	for (si = apkenv_solist; si != NULL; si = si->next) {
		if (si == handle)
			return true;
	}

	return false;
}

static inline int apkenv_validate_soinfo(soinfo *si)
{
	return (si >= apkenv_sopool && si < apkenv_sopool + SO_MAX) ||
	       si == &apkenv_libdl_info;
}

static char apkenv_ldpaths_buf[LDPATH_BUFSIZE];
static const char *apkenv_ldpaths[LDPATH_MAX + 1];

static const char *apkenv_ldpreload_names[LDPRELOAD_MAX + 1];

static soinfo *apkenv_preloads[LDPRELOAD_MAX + 1];

#if LINKER_DEBUG
int apkenv_debug_verbosity = 0;
#endif

static int apkenv_pid;

/* This boolean is set if the program being loaded is setuid */
static int apkenv_program_is_setuid;

#if STATS
struct _link_stats apkenv_linker_stats;
#endif

#if COUNT_PAGES
uint32_t apkenv_bitmask[4096];
#endif

#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001 /* .ARM.exidx segment */
#endif

static char apkenv_tmp_err_buf[768];
static char apkenv___linker_dl_err_buf[768];
#define DL_ERR(fmt, x...)                                                                     \
	do {                                                                                  \
		format_buffer(apkenv___linker_dl_err_buf, sizeof(apkenv___linker_dl_err_buf), \
			      "%s[%d]: " fmt, __func__, __LINE__, ##x);                       \
		ERROR(fmt "\n", ##x);                                                         \
	} while (0)

const char *apkenv_linker_get_error(void)
{
	return (const char *)&apkenv___linker_dl_err_buf[0];
}

enum {
	WRAPPER_DYNHOOK,
	WRAPPER_THUMB_INJECTION,
	WRAPPER_UNHOOKED,
	WRAPPER_ARM_INJECTION,
};

/*
 * This function is an empty stub where GDB locates a breakpoint to get notified
 * about linker activity.
 */
// extern void __attribute__((noinline)) rtld_db_dlactivity(void);

// static struct r_debug _r_debug = {1, NULL, &rtld_db_dlactivity,
//                                   RT_CONSISTENT, 0};
/* apkenv */
// this will be filled by the main executable (either to &_r_debug, or to the address taken from DT_DEBUG)
struct r_debug *_r_debug_ptr;

#define rtld_db_dlactivity() ((void (*)(void))_r_debug_ptr->r_brk)()

static struct link_map *apkenv_r_debug_head, *apkenv_r_debug_tail;

static pthread_mutex_t apkenv__r_debug_lock = PTHREAD_MUTEX_INITIALIZER;

static void apkenv_insert_soinfo_into_debug_map(soinfo *info)
{
	struct link_map *map;

	/* Copy the necessary fields into the debug structure.
	 */
	map = &(info->linkmap);
	map->l_addr = info->base;
	map->l_name = info->fullpath;
	map->l_ld = (void *)info->dynamic;

	/* Stick the new library at the end of the list.
	 * gdb tends to care more about libc than it does
	 * about leaf libraries, and ordering it this way
	 * reduces the back-and-forth over the wire.
	 */
	if (apkenv_r_debug_tail) {
		apkenv_r_debug_tail->l_next = map;
		map->l_prev = apkenv_r_debug_tail;
		map->l_next = 0;
	} else {
		/* apkenv: for sending to gdb */
		apkenv_r_debug_head = map;
		map->l_prev = 0;
		map->l_next = 0;
	}
	apkenv_r_debug_tail = map;
}

static void apkenv_remove_soinfo_from_debug_map(soinfo *info)
{
	struct link_map *map = &(info->linkmap);

	if (apkenv_r_debug_tail == map)
		apkenv_r_debug_tail = map->l_prev;

	if (map->l_prev)
		map->l_prev->l_next = map->l_next;
	if (map->l_next)
		map->l_next->l_prev = map->l_prev;
}

void apkenv_notify_gdb_of_libraries(void);

static void apkenv_notify_gdb_of_load(soinfo *info)
{
	if (info->flags & FLAG_EXE) {
		// GDB already knows about the main executable
		return;
	}

	pthread_mutex_lock(&apkenv__r_debug_lock);

	_r_debug_ptr->r_state = RT_ADD;
	rtld_db_dlactivity();

	apkenv_insert_soinfo_into_debug_map(info);

	_r_debug_ptr->r_state = RT_CONSISTENT;
	rtld_db_dlactivity();

	apkenv_notify_gdb_of_libraries();
	pthread_mutex_unlock(&apkenv__r_debug_lock);
}

static void apkenv_notify_gdb_of_unload(soinfo *info)
{
	if (info->flags & FLAG_EXE) {
		// GDB already knows about the main executable
		return;
	}

	pthread_mutex_lock(&apkenv__r_debug_lock);

	_r_debug_ptr->r_state = RT_DELETE;
	rtld_db_dlactivity();

	apkenv_remove_soinfo_from_debug_map(info);

	_r_debug_ptr->r_state = RT_CONSISTENT;
	rtld_db_dlactivity();

	apkenv_notify_gdb_of_libraries();
	pthread_mutex_unlock(&apkenv__r_debug_lock);
}

void apkenv_notify_gdb_of_libraries(void)
{
	struct link_map *tmap = _r_debug_ptr->r_map;
	while (tmap->l_next != NULL)
		tmap = tmap->l_next;

	_r_debug_ptr->r_state = RT_ADD;
	rtld_db_dlactivity();

	/* append android libs before notifying gdb */
	tmap->l_next = apkenv_r_debug_head;
	apkenv_r_debug_head->l_prev = tmap;

	_r_debug_ptr->r_state = RT_CONSISTENT;
	rtld_db_dlactivity();

	/* restore so that ld-linux doesn't freak out */
	tmap->l_next = NULL;
}

static soinfo *apkenv_alloc_info(const char *name)
{
	soinfo *si;

	if (strlen(name) >= SOINFO_NAME_LEN) {
		DL_ERR("%5d library name %s too long", apkenv_pid, name);
		return NULL;
	}

	/* The apkenv_freelist is populated when we call apkenv_free_info(), which in turn is
	   done only by dlclose(), which is not likely to be used.
	*/
	if (!apkenv_freelist) {
		if (apkenv_socount == SO_MAX) {
			DL_ERR("%5d too many libraries when loading %s", apkenv_pid, name);
			return NULL;
		}
		apkenv_freelist = apkenv_sopool + apkenv_socount++;
		apkenv_freelist->next = NULL;
	}

	si = apkenv_freelist;
	apkenv_freelist = apkenv_freelist->next;

	/* Make sure we get a clean block of soinfo */
	memset(si, 0, sizeof(soinfo));
	apkenv_strlcpy((char *)si->name, name, sizeof(si->name));
	apkenv_sonext->next = si;
	si->next = NULL;
	si->refcount = 0;
	apkenv_sonext = si;

	TRACE("%5d name %s: allocated soinfo @ %p\n", apkenv_pid, name, si);
	return si;
}

static void apkenv_free_info(soinfo *si)
{
	soinfo *prev = NULL, *trav;

	TRACE("%5d name %s: freeing soinfo @ %p\n", apkenv_pid, si->name, si);

	for (trav = apkenv_solist; trav != NULL; trav = trav->next) {
		if (trav == si)
			break;
		prev = trav;
	}
	if (trav == NULL) {
		/* si was not ni apkenv_solist */
		DL_ERR("%5d name %s is not in apkenv_solist!", apkenv_pid, si->name);
		return;
	}

	/* prev will never be NULL, because the first entry in apkenv_solist is
	   always the static apkenv_libdl_info.
	*/
	prev->next = si->next;
	if (si == apkenv_sonext)
		apkenv_sonext = prev;
	si->next = apkenv_freelist;
	apkenv_freelist = si;
}

/* For a given PC, find the .so that it belongs to.
 * Returns the base address of the .ARM.exidx section
 * for that .so, and the number of 8-byte entries
 * in that section (via *pcount).
 *
 * Intended to be called by libc's __gnu_Unwind_Find_exidx().
 *
 * This function is exposed via dlfcn.c and libdl.so.
 */
#if defined(__arm__)
_Unwind_Ptr bionic_dl_unwind_find_exidx(_Unwind_Ptr pc, int *pcount)
{
	soinfo *si;
	intptr_t addr = (intptr_t)pc;
	for (si = apkenv_solist; si != 0; si = si->next) {
		if ((addr >= si->base) && (addr < (si->base + si->size))) {
			*pcount = si->ARM_exidx_count;
			return (_Unwind_Ptr)(si->base + (uint64_t)si->ARM_exidx);
		}
	}
	*pcount = 0;
	return NULL;
}
#elif defined(__aarch64__) || defined(__i386__) || defined(__mips__) || defined(__x86_64__)
/* Here, we only have to provide a callback to iterate across all the
 * loaded libraries. gcc_eh does the rest. */
int bionic_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *info, size_t size, void *data),
			   void *data)
{
	soinfo *si;
	struct dl_phdr_info dl_info;
	int rv = 0;
	for (si = apkenv_solist; si != NULL; si = si->next) {
		dl_info.dlpi_addr = si->linkmap.l_addr;
		dl_info.dlpi_name = si->linkmap.l_name;
		dl_info.dlpi_phdr = (void *)si->phdr;
		dl_info.dlpi_phnum = si->phnum;
		if ((rv = cb(&dl_info, sizeof(struct dl_phdr_info), data)) != 0)
			break;
	}
	return rv;
}
#endif

static inline bool is_gnu_hash(soinfo *si)
{
	return (si->flags & FLAG_GNU_HASH);
}

static uint32_t apkenv_sysvhash(struct symbol_name *symbol_name)
{
	if(!symbol_name->has_sysv_hash) {
		const unsigned char *name = (const unsigned char *)symbol_name->name;
		uint32_t h = 0, g;

		while (*name) {
			h = (h << 4) + *name++;
			g = h & 0xf0000000;
			h ^= g;
			h ^= g >> 24;
		}

		symbol_name->sysv_hash = h;
	}

	return symbol_name->sysv_hash;
}

static uint32_t apkenv_gnuhash(struct symbol_name *symbol_name)
{
	if(!symbol_name->has_gnu_hash) {
		uint32_t h = 5381;
		const unsigned char* name = (const unsigned char *)(symbol_name->name);
		while (*name != 0) {
			h += (h << 5) + *name++; // h*33 + c = h + h * 32 + c = h + h << 5 + c
		}

		symbol_name->gnu_hash = h;
	}

	return symbol_name->gnu_hash;
}

static bool is_symbol_global_and_defined(const soinfo *si, const ElfW(Sym) *s) {
	/* only concern ourselves with global and weak symbol definitions */
	if (ELF_ST_BIND(s->st_info) == STB_GLOBAL ||
	    ELF_ST_BIND(s->st_info) == STB_WEAK) {
		return s->st_shndx != SHN_UNDEF;
	} else if (ELF_ST_BIND(s->st_info) != STB_LOCAL) {
		WARN("unexpected ST_BIND value: %d for '%s' in '%s'",
		     ELF_ST_BIND(s->st_info), si->strtab + s->st_name, si->name);
	}

	return false;
}

static ElfW(Sym) * apkenv__elf_lookup_sysv(soinfo *si, struct symbol_name *symbol_name)
{
	const char *name = symbol_name->name;
	uint32_t hash = apkenv_sysvhash(symbol_name);

	TRACE_TYPE(LOOKUP, "SEARCH %s in %s@%p h=%x(elf) %zd",
	           name, si->name, (void *)si->base, hash, hash % si->nbucket);

	for (uint32_t n = si->bucket[hash % si->nbucket]; n != 0; n = si->chain[n]) {
		ElfW(Sym) *s = si->symtab + n;
		if (!strcmp(si->strtab + s->st_name, name) && is_symbol_global_and_defined(si, s)) {
			TRACE_TYPE(LOOKUP, "FOUND %s in %s (%p) %zd",
			           name, si->name, (void *)(s->st_value), (size_t)(s->st_size));
			return s;
		}
	}

	TRACE_TYPE(LOOKUP, "NOT FOUND %s in %s@%p h=%x(elf) %zd",
	           symbol_name->name, si->name, (void *)si->base, hash, hash % si->nbucket);

	return NULL;
}

static ElfW(Sym) * apkenv__elf_lookup_gnu(soinfo *si, struct symbol_name *symbol_name)
{
	const char *name = symbol_name->name;
	uint32_t hash = apkenv_gnuhash(symbol_name);
	uint32_t h2 = hash >> si->gnu_shift2;
	uint32_t bloom_mask_bits = sizeof(ElfW(Addr))*8;
	uint32_t word_num = (hash / bloom_mask_bits) & si->gnu_maskwords;
	ElfW(Addr) bloom_word = si->gnu_bloom_filter[word_num];

	TRACE_TYPE(LOOKUP, "SEARCH %s in %s@%p h=%x(gnu) %zd",
	           name, si->name, (void *)si->base, hash, hash % si->nbucket);

	// test against bloom filter
	if ((1 & (bloom_word >> (hash % bloom_mask_bits)) & (bloom_word >> (h2 % bloom_mask_bits))) == 0)
		return NULL;

	// bloom test says "probably yes"...
	uint32_t n = si->bucket[hash % si->nbucket];
	if (n == 0)
		return NULL;

	do {
		ElfW(Sym)* s = si->symtab + n;
		if (((si->chain[n] ^ hash) >> 1) == 0 &&
		    strcmp(si->strtab + s->st_name, name) == 0 &&
		    is_symbol_global_and_defined(si, s)) {
			TRACE_TYPE(LOOKUP, "FOUND %s in %s (%p) %zd",
			           name, si->name, (void *)(s->st_value), (size_t)(s->st_size));
			return s;
		}
	} while ((si->chain[n++] & 1) == 0);

	TRACE_TYPE(LOOKUP, "NOT FOUND %s in %s@%p h=%x(gnu) %zd",
	           symbol_name->name, si->name, (void *)si->base, hash, hash % si->nbucket);

	return NULL;
}

static ElfW(Sym) * apkenv__elf_lookup(soinfo *si, struct symbol_name *symbol_name)
{
	return is_gnu_hash(si) ? apkenv__elf_lookup_gnu(si, symbol_name) : apkenv__elf_lookup_sysv(si, symbol_name);
}

const char *apkenv_last_library_used = NULL;

static ElfW(Sym) *
    apkenv__do_lookup(soinfo *si, const char *name, ElfW(Addr) * base)
{
	struct symbol_name symbol_name = { .name = name };
	ElfW(Sym) * s;
	soinfo *lsi = si;
	int i;

	/* Look for symbols in the local scope (the object who is
	 * searching). This happens with C++ templates on i386 for some
	 * reason.
	 *
	 * Notes on weak symbols:
	 * The ELF specs are ambigious about treatment of weak definitions in
	 * dynamic linking.  Some systems return the first definition found
	 * and some the first non-weak definition.   This is system dependent.
	 * Here we return the first definition found for simplicity.  */

	s = apkenv__elf_lookup(si, &symbol_name);
	if (s != NULL)
		goto done;

	/* Next, look for it in the apkenv_preloads list */
	for (i = 0; apkenv_preloads[i] != NULL; i++) {
		lsi = apkenv_preloads[i];
		s = apkenv__elf_lookup(lsi, &symbol_name);
		if (s != NULL)
			goto done;
	}

	for (ElfW(Dyn) *d = si->dynamic; d->d_tag != DT_NULL; d++) {
		if (d->d_tag == DT_NEEDED) {
			lsi = (soinfo *)d->d_un.d_val;
			if (!apkenv_validate_soinfo(lsi)) {
				DL_ERR("%5d bad DT_NEEDED pointer in %s",
				       apkenv_pid, si->name);
				return NULL;
			}

			DEBUG("%5d %s: looking up %s in %s\n",
			      apkenv_pid, si->name, name, lsi->name);
			s = apkenv__elf_lookup(lsi, &symbol_name);
			if ((s != NULL) && (s->st_shndx != SHN_UNDEF))
				goto done;
		}
	}

#if ALLOW_SYMBOLS_FROM_MAIN
	/* If we are resolving relocations while dlopen()ing a library, it's OK for
	 * the library to resolve a symbol that's defined in the executable itself,
	 * although this is rare and is generally a bad idea.
	 */
	if (apkenv_somain) {
		lsi = apkenv_somain;
		DEBUG("%5d %s: looking up %s in executable %s\n",
		      apkenv_pid, si->name, name, lsi->name);
		s = apkenv__elf_lookup(lsi, &symbol_name);
	}
#endif

done:
	if (s != NULL) {
		TRACE_TYPE(LOOKUP, "%5d si %s sym %s s->st_value = 0x%016lx, "
				   "found in %s, base = 0x%016lx\n",
			   apkenv_pid, si->name, name, s->st_value, lsi->name, lsi->base);
		apkenv_last_library_used = lsi->name;
		*base = lsi->base;
		return s;
	}

	return NULL;
}

/* This is used by dl_sym().  It performs symbol lookup only within the
   specified soinfo object and not in any of its dependencies.
 */
ElfW(Sym) * apkenv_lookup_in_library(soinfo *si, const char *name)
{
	return apkenv__elf_lookup(si, &(struct symbol_name){ .name = name });
}

/* This is used by dl_sym().  It performs a global symbol lookup.
 */
ElfW(Sym) * apkenv_lookup(const char *name, soinfo **found, soinfo *start)
{
	struct symbol_name symbol_name = { .name = name };
	ElfW(Sym) *s = NULL;
	soinfo *si;

	if (start == NULL) {
		start = apkenv_solist;
	}

	for (si = start; (s == NULL) && (si != NULL); si = si->next) {
		if (si->flags & FLAG_ERROR)
			continue;
		s = apkenv__elf_lookup(si, &symbol_name);
		if (s != NULL) {
			*found = si;
			break;
		}
	}

	if (s != NULL) {
		TRACE_TYPE(LOOKUP, "%5d %s s->st_value = 0x%016lx, "
				   "si->base = 0x%016lx\n",
			   apkenv_pid, name, s->st_value, si->base);
		return s;
	}

	return NULL;
}

soinfo *apkenv_find_containing_library(const void *addr)
{
	soinfo *si;

	for (si = apkenv_solist; si != NULL; si = si->next) {
		if ((intptr_t)addr >= si->base && (intptr_t)addr - si->base < si->size) {
			return si;
		}
	}

	return NULL;
}

static bool symbol_matches_soaddr(const ElfW(Sym)* sym, ElfW(Addr) soaddr) {
	return sym->st_shndx != SHN_UNDEF &&
	       soaddr >= sym->st_value &&
	       soaddr < sym->st_value + sym->st_size;
}

ElfW(Sym) * apkenv_find_containing_symbol_sysv(const void *addr, soinfo *si)
{
	ElfW(Addr) soaddr = (ElfW(Addr))(addr) - si->base;

	/* Search the library's symbol table for any defined symbol which
	 * contains this address */
	for (size_t i = 0; i < si->nchain; i++) {
		ElfW(Sym) *sym = &si->symtab[i];

		if (symbol_matches_soaddr(sym, soaddr)) {
			return sym;
		}
	}

	return NULL;
}

ElfW(Sym) * apkenv_find_containing_symbol_gnu(const void *addr, soinfo *si)
{
	ElfW(Addr) soaddr = (ElfW(Addr))(addr) - si->base;

	for (size_t i = 0; i < si->nbucket; ++i) {
		uint32_t n = si->bucket[i];

		if (n == 0) {
			continue;
		}

		do {
			ElfW(Sym)* sym = si->symtab + n;
			if (symbol_matches_soaddr(sym, soaddr)) {
				return sym;
			}
		} while ((si->chain[n++] & 1) == 0);
	}

	return NULL;
}

ElfW(Sym) * apkenv_find_containing_symbol(const void *addr, soinfo *si)
{
	return is_gnu_hash(si) ? apkenv_find_containing_symbol_gnu(addr, si) : apkenv_find_containing_symbol_sysv(addr, si);
}

#if 0
static void dump(soinfo *si)
{
	ElfW(Sym) *s = si->symtab;
	unsigned n;

	for(n = 0; n < si->nchain; n++) {
		TRACE("%5d %04d> %016lx: %02x %04x %016lx %016lx %s\n", apkenv_pid, n, s,
		       s->st_info, s->st_shndx, s->st_value, s->st_size,
		       si->strtab + s->st_name);
		s++;
	}
}
#endif

#define SOPATH_MAX 8

static const char *apkenv_sopaths[SOPATH_MAX + 1] = {
	"/vendor/lib",
	"/system/lib",
	/* APKENV_PREFIX "/lib/" APKENV_TARGET "/bionic/", */
#ifdef APKENV_LOCAL_BIONIC_PATH
	APKENV_LOCAL_BIONIC_PATH,
#endif
};

int apkenv_add_sopath(const char *path)
{
	int i;
	for (i = 0; i < SOPATH_MAX; i++) {
		if (apkenv_sopaths[i] == NULL) {
			apkenv_sopaths[i] = path;
			return 0;
		}

		if (strcmp(path, apkenv_sopaths[i]) == 0)
			return 0;
	}

	ERROR("too many apkenv_sopaths\n");
	return -1;
}

static int apkenv__open_lib(const char *name)
{
	int fd;
	struct stat filestat;

	TRACE("[ %5d apkenv__open_lib called with %s ]\n", apkenv_pid, name);

	if ((stat(name, &filestat) >= 0) && S_ISREG(filestat.st_mode)) {
		if ((fd = open(name, O_RDONLY)) >= 0)
			return fd;
	}

	return -1;
}

static int apkenv_open_library(const char *name, char *fullpath)
{
	int fd;
	char buf[512];
	const char **path;
	int n;

	char *path_normalized_name;
	char *tmp_name;

	TRACE("[ %5d opening %s ]\n", apkenv_pid, name);

	if (name == 0)
		return -1;
	if (strlen(name) > 256)
		return -1;

	strcpy(fullpath, name);

	tmp_name = strdup(name);
	path_normalized_name = realpath(dirname(tmp_name), NULL);
	free(tmp_name);

	for (path = apkenv_ldpaths; *path; path++) {
		char *ldpath_normalized = realpath(*path, NULL);
		if (!ldpath_normalized)
			ldpath_normalized = strdup(*path);
		if (path_normalized_name && ldpath_normalized) {
			TRACE("[ %5d comparing '%s' against '%s' to see if the libary is in BIONIC_LD_LIBRARY_PATH ]\n", apkenv_pid, path_normalized_name, ldpath_normalized);
			if (!fnmatch(ldpath_normalized, path_normalized_name, 0)) {
				if ((fd = apkenv__open_lib(name)) >= 0) {
					free(ldpath_normalized);
					free(path_normalized_name);
					return fd;
				}
			}
			free(ldpath_normalized);
		} else {
			WARN("realpath returned NULL: can't compare path_normalized_name('%s') vs ldpath_normalized('%s')\n", path_normalized_name, ldpath_normalized);
		}

		n = format_buffer(buf, sizeof(buf), "%s/%s", *path, name);
		if (n < 0 || n >= (int)sizeof(buf)) {
			WARN("Ignoring very long library path: %s/%s\n", *path, name);
			continue;
		}
		if ((fd = apkenv__open_lib(buf)) >= 0) {
			strcpy(fullpath, buf);
			if (path_normalized_name)
				free(path_normalized_name);
			return fd;
		}
	}

	free(path_normalized_name);

	for (path = apkenv_sopaths; *path; path++) {
		n = format_buffer(buf, sizeof(buf), "%s/%s", *path, name);
		if (n < 0 || n >= (int)sizeof(buf)) {
			WARN("Ignoring very long library path: %s/%s\n", *path, name);
			continue;
		}
		if ((fd = apkenv__open_lib(buf)) >= 0) {
			strcpy(fullpath, buf);
			return fd;
		}
	}

	return -1;
}

typedef struct {
	long mmap_addr;
	char tag[4]; /* 'P', 'R', 'E', ' ' */
} prelink_info_t;

/* Returns the requested base address if the library is prelinked,
 * and 0 otherwise.  */
static unsigned long
apkenv_is_prelinked(int fd, const char *name)
{
	off_t sz;
	prelink_info_t info;

	sz = lseek(fd, -sizeof(prelink_info_t), SEEK_END);
	if (sz < 0) {
		DL_ERR("lseek() failed!");
		return 0;
	}

	if (read(fd, &info, sizeof(info)) != sizeof(info)) {
		WARN("Could not read prelink_info_t structure for `%s`\n", name);
		return 0;
	}

	if (strncmp(info.tag, "PRE ", 4)) {
		WARN("`%s` is not a prelinked library\n", name);
		return 0;
	}

	return (unsigned long)info.mmap_addr;
}

/* apkenv_verify_elf_object
 *      Verifies if the object @ base is a valid ELF object
 *
 * Args:
 *
 * Returns:
 *       0 on success
 *      -1 if no valid ELF object is found @ base.
 */
static int
apkenv_verify_elf_object(void *base, const char *name)
{
	ElfW(Ehdr) *hdr = (ElfW(Ehdr) *)base;

	if (hdr->e_ident[EI_MAG0] != ELFMAG0)
		return -1;
	if (hdr->e_ident[EI_MAG1] != ELFMAG1)
		return -1;
	if (hdr->e_ident[EI_MAG2] != ELFMAG2)
		return -1;
	if (hdr->e_ident[EI_MAG3] != ELFMAG3)
		return -1;

	/* TODO: make this proper for aarch64 / x86_64 */
	/*#ifdef ANDROID_ARM_LINKER
	    if (hdr->e_machine != EM_ARM) return -1;
	#elif defined(ANDROID_X86_LINKER)
	    if (hdr->e_machine != EM_386) return -1;
	#endif*/
	return 0;
}

/* apkenv_get_lib_extents
 *      Retrieves the base (*base) address where the ELF object should be
 *      mapped and its overall memory size (*total_sz).
 *
 * Args:
 *      fd: Opened file descriptor for the library
 *      name: The name of the library
 *      _hdr: Pointer to the header page of the library
 *      total_sz: Total size of the memory that should be allocated for
 *                this library
 *
 * Returns:
 *      -1 if there was an error while trying to get the lib extents.
 *         The possible reasons are:
 *             - Could not determine if the library was prelinked.
 *             - The library provided is not a valid ELF object
 *       0 if the library did not request a specific base offset (normal
 *         for non-prelinked libs)
 *     > 0 if the library requests a specific address to be mapped to.
 *         This indicates a pre-linked library.
 */
static intptr_t
apkenv_get_lib_extents(int fd, const char *name, void *__hdr, size_t *total_sz)
{
	intptr_t req_base;
	intptr_t min_vaddr = 0xffffffff;
	intptr_t max_vaddr = 0;
	unsigned char *_hdr = (unsigned char *)__hdr;
	ElfW(Ehdr) *ehdr = (ElfW(Ehdr) *)_hdr;
	ElfW(Phdr) * phdr;
	int cnt;

	TRACE("[ %5d Computing extents for '%s'. ]\n", apkenv_pid, name);
	if (apkenv_verify_elf_object(_hdr, name) < 0) {
		DL_ERR("%5d - %s is not a valid ELF object", apkenv_pid, name);
		return (intptr_t)-1;
	}

	req_base = (intptr_t)apkenv_is_prelinked(fd, name);
	if (req_base == (intptr_t)-1)
		return -1;
	else if (req_base != 0) {
		TRACE("[ %5d - Prelinked library '%s' requesting base @ 0x%016lx ]\n",
		      apkenv_pid, name, req_base);
	} else {
		TRACE("[ %5d - Non-prelinked library '%s' found. ]\n", apkenv_pid, name);
	}

	phdr = (ElfW(Phdr) *)(_hdr + ehdr->e_phoff);

	/* find the min/max p_vaddrs from all the PT_LOAD segments so we can
	 * get the range. */
	for (cnt = 0; cnt < ehdr->e_phnum; ++cnt, ++phdr) {
		if (phdr->p_type == PT_LOAD) {
			if ((phdr->p_vaddr + phdr->p_memsz) > max_vaddr)
				max_vaddr = phdr->p_vaddr + phdr->p_memsz;
			if (phdr->p_vaddr < min_vaddr)
				min_vaddr = phdr->p_vaddr;
		}
	}

	if ((min_vaddr == 0xffffffff) && (max_vaddr == 0)) {
		DL_ERR("%5d - No loadable segments found in %s.", apkenv_pid, name);
		return (intptr_t)-1;
	}

	/* truncate min_vaddr down to page boundary */
	min_vaddr &= ~PAGE_MASK;

	/* round max_vaddr up to the next page */
	max_vaddr = (max_vaddr + PAGE_SIZE - 1) & ~PAGE_MASK;

	*total_sz = (max_vaddr - min_vaddr);
	return (intptr_t)req_base;
}

/* apkenv_reserve_mem_region
 *
 *     This function reserves a chunk of memory to be used for mapping in
 *     a prelinked shared library. We reserve the entire memory region here, and
 *     then the rest of the linker will relocate the individual loadable
 *     segments into the correct locations within this memory range.
 *
 * Args:
 *     si->base: The requested base of the allocation.
 *     si->size: The size of the allocation.
 *
 * Returns:
 *     -1 on failure, and 0 on success.  On success, si->base will contain
 *     the virtual address at which the library will be mapped.
 */

static int apkenv_reserve_mem_region(soinfo *si)
{
	void *base = mmap((void *)si->base, si->size, PROT_NONE,
			  MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		DL_ERR("%5d can NOT map (%sprelinked) library '%s' at 0x%016lx "
		       "as requested, will try general pool: %d (%s)",
		       apkenv_pid, (si->base ? "" : "non-"), si->name, si->base,
		       errno, strerror(errno));
		return -1;
	} else if (base != (void *)si->base) {
		DL_ERR("OOPS: %5d %sprelinked library '%s' mapped at 0x%016lx, "
		       "not at 0x%016lx",
		       apkenv_pid, (si->base ? "" : "non-"),
		       si->name, (intptr_t)base, si->base);
		munmap(base, si->size);
		return -1;
	}
	return 0;
}

static int apkenv_alloc_mem_region(soinfo *si)
{
	if (si->base) {
		/* Attempt to mmap a prelinked library. */
		return apkenv_reserve_mem_region(si);
	}

	/* This is not a prelinked library, so we use the kernel's default
	   allocator.
	*/

	void *base = mmap(NULL, si->size, PROT_NONE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		DL_ERR("%5d mmap of library '%s' failed: %d (%s)\n",
		       apkenv_pid, si->name,
		       errno, strerror(errno));
		goto err;
	}
	si->base = (intptr_t)base;
	PRINT("%5d mapped library '%s' to %016lx via kernel allocator.\n",
	      apkenv_pid, si->name, si->base);
	return 0;

err:
	DL_ERR("OOPS: %5d cannot map library '%s'. no vspace available.",
	       apkenv_pid, si->name);
	return -1;
}

#define MAYBE_MAP_FLAG(x, from, to) (((x) & (from)) ? (to) : 0)
#define PFLAGS_TO_PROT(x)	    (MAYBE_MAP_FLAG((x), PF_X, PROT_EXEC) | \
			   MAYBE_MAP_FLAG((x), PF_R, PROT_READ) |           \
			   MAYBE_MAP_FLAG((x), PF_W, PROT_WRITE))
/* apkenv_load_segments
 *
 *     This function loads all the loadable (PT_LOAD) segments into memory
 *     at their appropriate memory offsets off the base address.
 *
 * Args:
 *     fd: Open file descriptor to the library to load.
 *     header: Pointer to a header page that contains the ELF header.
 *             This is needed since we haven't mapped in the real file yet.
 *     si: ptr to soinfo struct describing the shared object.
 *
 * Returns:
 *     0 on success, -1 on failure.
 */
static int
apkenv_load_segments(int fd, void *header, soinfo *si)
{
	ElfW(Ehdr) *ehdr = (ElfW(Ehdr) *)header;
	ElfW(Phdr) *phdr = (ElfW(Phdr) *)((unsigned char *)header + ehdr->e_phoff);
	ElfW(Addr) base = (ElfW(Addr))si->base;
	int cnt;
	size_t len;
	ElfW(Addr) tmp;
	unsigned char *pbase;
	unsigned char *extra_base;
	size_t extra_len;
	size_t total_sz = 0;

	si->wrprotect_start = 0xffffffff;
	si->wrprotect_end = 0;

	TRACE("[ %5d - Begin loading segments for '%s' @ 0x%016lx ]\n",
	      apkenv_pid, si->name, (intptr_t)si->base);
	/* Now go through all the PT_LOAD segments and map them into memory
	 * at the appropriate locations. */
	for (cnt = 0; cnt < ehdr->e_phnum; ++cnt, ++phdr) {
		if (phdr->p_type == PT_LOAD) {
			DEBUG_DUMP_PHDR(phdr, "PT_LOAD", apkenv_pid);
			/* we want to map in the segment on a page boundary */
			tmp = base + (phdr->p_vaddr & (~PAGE_MASK));
			/* add the # of bytes we masked off above to the total length. */
			len = phdr->p_filesz + (phdr->p_vaddr & PAGE_MASK);

			TRACE("[ %d - Trying to load segment from '%s' @ 0x%016lx "
			      "(0x%016lx). p_vaddr=0x%016lx p_offset=0x%016lx ]\n",
			      apkenv_pid, si->name,
			      tmp, len, phdr->p_vaddr, phdr->p_offset);
			pbase = mmap((void *)tmp, len, PFLAGS_TO_PROT(phdr->p_flags),
				     MAP_PRIVATE | MAP_FIXED, fd,
				     phdr->p_offset & (~PAGE_MASK));
			if (pbase == MAP_FAILED) {
				DL_ERR("%d failed to map segment from '%s' @ 0x%016lx (0x%016lx). "
				       "p_vaddr=0x%016lx p_offset=0x%016lx",
				       apkenv_pid, si->name,
				       tmp, len, phdr->p_vaddr, phdr->p_offset);
				goto fail;
			}

			/* If 'len' didn't end on page boundary, and it's a writable
			 * segment, zero-fill the rest. */
			if ((len & PAGE_MASK) && (phdr->p_flags & PF_W))
				memset((void *)(pbase + len), 0, PAGE_SIZE - (len & PAGE_MASK));

			/* Check to see if we need to extend the map for this segment to
			 * cover the diff between filesz and memsz (i.e. for bss).
			 *
			 *  base           _+---------------------+  page boundary
			 *                  .                     .
			 *                  |                     |
			 *                  .                     .
			 *  pbase          _+---------------------+  page boundary
			 *                  |                     |
			 *                  .                     .
			 *  base + p_vaddr _|                     |
			 *                  . \          \        .
			 *                  . | filesz   |        .
			 *  pbase + len    _| /          |        |
			 *     <0 pad>      .            .        .
			 *  extra_base     _+------------|--------+  page boundary
			 *               /  .            .        .
			 *               |  .            .        .
			 *               |  +------------|--------+  page boundary
			 *  extra_len->  |  |            |        |
			 *               |  .            | memsz  .
			 *               |  .            |        .
			 *               \ _|            /        |
			 *                  .                     .
			 *                  |                     |
			 *                 _+---------------------+  page boundary
			 */
			tmp = (ElfW(Addr))(((intptr_t)pbase + len + PAGE_SIZE - 1) &
					   (~PAGE_MASK));
			if (tmp < (base + phdr->p_vaddr + phdr->p_memsz)) {
				extra_len = base + phdr->p_vaddr + phdr->p_memsz - tmp;
				TRACE("[ %5d - Need to extend segment from '%s' @ 0x%016lx "
				      "(0x%016lx) ]\n",
				      apkenv_pid, si->name, tmp, extra_len);
				/* map in the extra page(s) as anonymous into the range.
				 * This is probably not necessary as we already mapped in
				 * the entire region previously, but we just want to be
				 * sure. This will also set the right flags on the region
				 * (though we can probably accomplish the same thing with
				 * mprotect).
				 */
				extra_base = mmap((void *)tmp, extra_len,
						  PFLAGS_TO_PROT(phdr->p_flags),
						  MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS,
						  -1, 0);
				if (extra_base == MAP_FAILED) {
					DL_ERR("[ %5d - failed to extend segment from '%s' @ 0x%016lx"
					       " (0x%016lx) ]",
					       apkenv_pid, si->name, tmp,
					       extra_len);
					goto fail;
				}
				/* TODO: Check if we need to memset-0 this region.
				 * Anonymous mappings are zero-filled copy-on-writes, so we
				 * shouldn't need to. */
				TRACE("[ %5d - Segment from '%s' extended @ 0x%016lx "
				      "(0x%016lx)\n",
				      apkenv_pid, si->name, (uint64_t)extra_base,
				      extra_len);
			}
			/* set the len here to show the full extent of the segment we
			 * just loaded, mostly for debugging */
			len = (((intptr_t)base + phdr->p_vaddr + phdr->p_memsz +
				PAGE_SIZE - 1) &
			       (~PAGE_MASK)) -
			      (intptr_t)pbase;
			TRACE("[ %5d - Successfully loaded segment from '%s' @ 0x%016lx "
			      "(0x%016lx). p_vaddr=0x%016lx p_offset=0x%016lx\n",
			      apkenv_pid, si->name,
			      (uint64_t)pbase, len, phdr->p_vaddr, phdr->p_offset);
			total_sz += len;
			/* Make the section writable just in case we'll have to write to
			 * it during relocation (i.e. text segment). However, we will
			 * remember what range of addresses should be write protected.
			 *
			 */
			if (!(phdr->p_flags & PF_W)) {
				if ((intptr_t)pbase < si->wrprotect_start)
					si->wrprotect_start = (intptr_t)pbase;
				if (((intptr_t)pbase + len) > si->wrprotect_end)
					si->wrprotect_end = (intptr_t)pbase + len;
				mprotect(pbase, len,
					 PFLAGS_TO_PROT(phdr->p_flags) | PROT_WRITE);
			}
		} else if (phdr->p_type == PT_DYNAMIC) {
			DEBUG_DUMP_PHDR(phdr, "PT_DYNAMIC", apkenv_pid);
			/* this segment contains the dynamic linking information */
			si->dynamic = (ElfW(Dyn) *)(base + phdr->p_vaddr);
		} else if (phdr->p_type == PT_GNU_RELRO) {
			if ((phdr->p_vaddr >= si->size) || ((phdr->p_vaddr + phdr->p_memsz) > si->size) || ((base + phdr->p_vaddr + phdr->p_memsz) < base)) {
				DL_ERR("%d invalid GNU_RELRO in '%s' "
				       "p_vaddr=0x%016lx p_memsz=0x%016lx",
				       apkenv_pid, si->name,
				       phdr->p_vaddr, phdr->p_memsz);
				goto fail;
			}
			si->gnu_relro_start = (ElfW(Addr))(base + phdr->p_vaddr);
			si->gnu_relro_len = (size_t)phdr->p_memsz;
		} else {
#if defined(__arm__)
			if (phdr->p_type == PT_ARM_EXIDX) {
				DEBUG_DUMP_PHDR(phdr, "PT_ARM_EXIDX", apkenv_pid);
				/* exidx entries (used for stack unwinding) are 8 bytes each.
				 */
				si->ARM_exidx = (ElfW(Addr) *)phdr->p_vaddr;
				si->ARM_exidx_count = phdr->p_memsz / 8;
			}
#endif
		}
	}

	/* Sanity check */
	if (total_sz > si->size) {
		DL_ERR("%5d - Total length (0x%016lx) of mapped segments from '%s' is "
		       "greater than what was allocated (0x%016lx). THIS IS BAD!",
		       apkenv_pid, total_sz, si->name, si->size);
		goto fail;
	}

	TRACE("[ %5d - Finish loading segments for '%s' @ 0x%016lx. "
	      "Total memory footprint: 0x%016lx bytes ]\n",
	      apkenv_pid, si->name,
	      si->base, si->size);
	return 0;

fail:
	/* We can just blindly unmap the entire region even though some things
	 * were mapped in originally with anonymous and others could have been
	 * been mapped in from the file before we failed. The kernel will unmap
	 * all the pages in the range, irrespective of how they got there.
	 */
	munmap((void *)si->base, si->size);
	si->flags |= FLAG_ERROR;
	return -1;
}

/* TODO: Implement this to take care of the fact that Android ARM
 * ELF objects shove everything into a single loadable segment that has the
 * write bit set. wr_offset is then used to set non-(data|bss) pages to be
 * non-writable.
 */
#if 0
static unsigned
get_wr_offset(int fd, const char *name, ElfW(Ehdr) *ehdr)
{
    ElfW(Shdr) *shdr_start;
    ElfW(Shdr) *shdr;
    int shdr_sz = ehdr->e_shnum * sizeof(ElfW(Shdr));
    int cnt;
    unsigned wr_offset = 0xffffffff;

    shdr_start = mmap(0, shdr_sz, PROT_READ, MAP_PRIVATE, fd,
                      ehdr->e_shoff & (~PAGE_MASK));
    if (shdr_start == MAP_FAILED) {
        WARN("%5d - Could not read section header info from '%s'. Will not "
             "not be able to determine write-protect offset.\n", apkenv_pid, name);
        return (unsigned)-1;
    }

    for(cnt = 0, shdr = shdr_start; cnt < ehdr->e_shnum; ++cnt, ++shdr) {
        if ((shdr->sh_type != SHT_NULL) && (shdr->sh_flags & SHF_WRITE) &&
            (shdr->sh_addr < wr_offset)) {
            wr_offset = shdr->sh_addr;
        }
    }

    munmap(shdr_start, shdr_sz);
    return wr_offset;
}
#endif

static soinfo *
apkenv_load_library(const char *name, const bool try_glibc, int glibc_flag, void **_glibc_handle)
{
	char fullpath[512];
	int fd = apkenv_open_library(name, fullpath);
	int cnt;
	size_t ext_sz;
	size_t req_base;
	const char *bname;
	soinfo *si = NULL;
	ElfW(Ehdr) * hdr;

	if (fd == -1) {
		if (try_glibc) {
			if (!(glibc_flag & (RTLD_LAZY | RTLD_NOW)))
				glibc_flag |= RTLD_NOW;
			if (!_glibc_handle)
				glibc_flag |= RTLD_GLOBAL;
			void *glibc_handle;
			if (glibc_handle = dlopen(name, glibc_flag)) {
				if (_glibc_handle)
					*_glibc_handle = glibc_handle;
				DEBUG("Loaded %s with glibc dlopen\n", name);
				return NULL;
			} else {
				DEBUG("failed to load %s with glibc dlopen (error: %s)\n", name, dlerror());
			}
		}
		DL_ERR("shim bionic linker: library '%s' not found", name);

		return NULL;
	}

	off_t end;
	if ((end = lseek(fd, 0, SEEK_END)) < 0) {
		DL_ERR("lseek() failed!");
		goto fail;
	}

	uint8_t *bytes;
	if (!(bytes = calloc(1, end))) {
		DL_ERR("calloc() failed!");
		goto fail;
	}

	/* We have to read the ELF header to figure out what to do with this image
	 */
	if (lseek(fd, 0, SEEK_SET) < 0) {
		DL_ERR("lseek() failed!");
		goto fail;
	}

	if ((cnt = read(fd, bytes, end)) < 0) {
		DL_ERR("read() failed!");
		goto fail;
	}

	/* Parse the ELF header and get the size of the memory footprint for
	 * the library */
	req_base = apkenv_get_lib_extents(fd, name, bytes, &ext_sz);
	if (req_base == (intptr_t)-1)
		goto fail;
	TRACE("[ %5d - '%s' (%s) wants base=0x%016lx sz=0x%016lx ]\n", apkenv_pid, name,
	      (req_base ? "prelinked" : "not pre-linked"), req_base, ext_sz);

	/* Now configure the soinfo struct where we'll store all of our data
	 * for the ELF object. If the loading fails, we waste the entry, but
	 * same thing would happen if we failed during linking. Configuring the
	 * soinfo struct here is a lot more convenient.
	 */
	bname = strrchr(name, '/');
	si = apkenv_alloc_info(bname ? bname + 1 : name);
	if (si == NULL)
		goto fail;

	/* apkenv */
	strncpy(si->fullpath, fullpath, sizeof(si->fullpath));
	si->fullpath[sizeof(si->fullpath) - 1] = 0;

	/* Carve out a chunk of memory where we will map in the individual
	 * segments */
	si->base = req_base;
	si->size = ext_sz;
	si->flags = 0;
	si->entry = 0;
	si->dynamic = (ElfW(Dyn) *)-1;
	if (apkenv_alloc_mem_region(si) < 0)
		goto fail;

	TRACE("[ %5d allocated memory for %s @ %p (0x%016lx) ]\n",
	      apkenv_pid, name, (void *)si->base, ext_sz);

	/* Now actually load the library's segments into right places in memory */
	if (apkenv_load_segments(fd, bytes, si) < 0) {
		goto fail;
	}

	/* this might not be right. Technically, we don't even need this info
	 * once we go through 'apkenv_load_segments'. */
	hdr = (ElfW(Ehdr) *)si->base;
	si->phdr = (ElfW(Phdr) *)((unsigned char *)si->base + hdr->e_phoff);
	si->phnum = hdr->e_phnum;
	/**/

	close(fd);
	return si;

fail:
	if (si)
		apkenv_free_info(si);
	close(fd);
	return NULL;
}

static soinfo *
apkenv_init_library(soinfo *si)
{
	unsigned wr_offset = 0xffffffff; // this is not used...?

	/* At this point we know that whatever is loaded @ base is a valid ELF
	 * shared library whose segments are properly mapped in. */
	TRACE("[ %5d apkenv_init_library base=0x%016lx sz=0x%016lx name='%s') ]\n",
	      apkenv_pid, si->base, si->size, si->name);
	if (apkenv_link_image(si, wr_offset)) {
		/* We failed to link.  However, we can only restore libbase
		** if no additional libraries have moved it since we updated it.
		*/
		munmap((void *)si->base, si->size);
		return NULL;
	}

	return si;
}

soinfo *apkenv_find_library(const char *name, const bool try_glibc, int glibc_flag, void **glibc_handle)
{
	soinfo *si;
	const char *bname;

#if ALLOW_SYMBOLS_FROM_MAIN
	if (name == NULL)
		return apkenv_somain;
#else
	if (name == NULL)
		return NULL;
#endif

	// some libraries have `/system/lib/{name}` in DT_NEEDED, just strip the prefix
	const char *prefix = "/system/lib/";
	const int prefix_len = strlen(prefix);
	const char *prefix64 = "/system/lib64/";
	const int prefix64_len = strlen(prefix64);

	if(!strncmp(name, prefix, prefix_len))
		name += prefix_len;
	else if(!strncmp(name, prefix64, prefix64_len))
		name += prefix64_len;

	for(int i = 0; i < lib_override_map_len; i++) {
		if(!strcmp(lib_override_map[i].from, name)) {
			name = lib_override_map[i].to;
			break;
		}
	}

	bname = strrchr(name, '/');
	bname = bname ? bname + 1 : name;

	for (si = apkenv_solist; si != 0; si = si->next) {
		if (!strcmp(bname, si->name)) {
			if (si->flags & FLAG_ERROR) {
				DL_ERR("%5d '%s' failed to load previously", apkenv_pid, bname);
				return NULL;
			}
			if (si->flags & FLAG_LINKED)
				return si;
			DL_ERR("OOPS: %5d recursive link to '%s'", apkenv_pid, si->name);
			return NULL;
		}
	}

	TRACE("[ %5d '%s' has not been loaded yet.  Locating...]\n", apkenv_pid, name);
	if (!(si = apkenv_load_library(name, try_glibc, glibc_flag, glibc_handle)) || !(si = apkenv_init_library(si)))
		return NULL;

	if (!strcmp(bname, "libstdc++.so")) {
		ElfW(Sym) *sym = apkenv_lookup_in_library(si, "__cxa_demangle");
		if (sym && ELF_ST_BIND(sym->st_info) == STB_GLOBAL && sym->st_shndx != 0)
			wrapper_set_cpp_demangler((void *)(intptr_t)(sym->st_value + si->base));
	}

	return si;
}

/* TODO:
 *   notify gdb of unload
 *   for non-prelinked libraries, find a way to decrement libbase
 */
static void apkenv_call_destructors(soinfo *si);
unsigned int apkenv_unload_library(soinfo *si)
{
	if (si->refcount == 1) {
		TRACE("%5d unloading '%s'\n", apkenv_pid, si->name);
		apkenv_call_destructors(si);

		/*
		 * Make sure that we undo the PT_GNU_RELRO protections we added
		 * in apkenv_link_image. This is needed to undo the DT_NEEDED hack below.
		 */
		if ((si->gnu_relro_start != 0) && (si->gnu_relro_len != 0)) {
			ElfW(Addr) start = (si->gnu_relro_start & ~PAGE_MASK);
			size_t len = (si->gnu_relro_start - start) + si->gnu_relro_len;
			if (mprotect((void *)start, len, PROT_READ | PROT_WRITE) < 0)
				DL_ERR("%5d %s: could not undo GNU_RELRO protections. "
				       "Expect a crash soon. errno=%d (%s)",
				       apkenv_pid, si->name, errno, strerror(errno));
		}

		for (ElfW(Dyn) *d = si->dynamic; d->d_tag != DT_NULL; d++) {
			if (d->d_tag == DT_NEEDED) {
				soinfo *lsi = (soinfo *)d->d_un.d_val;

				// The next line will segfault if the we don't undo the
				// PT_GNU_RELRO protections (see comments above and in
				// apkenv_link_image().
				d->d_un.d_val = 0;

				if (apkenv_validate_soinfo(lsi)) {
					TRACE("%5d %s needs to unload %s\n", apkenv_pid,
					      si->name, lsi->name);
					apkenv_unload_library(lsi);
				} else
					DL_ERR("%5d %s: could not unload dependent library",
					       apkenv_pid, si->name);
			}
		}

		munmap((char *)si->base, si->size);
		apkenv_notify_gdb_of_unload(si);
		apkenv_free_info(si);
		si->refcount = 0;
	} else {
		si->refcount--;
		PRINT("%5d not unloading '%s', decrementing refcount to %lu\n",
		      apkenv_pid, si->name, si->refcount);
	}
	return si->refcount;
}

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* --- TODO: move this into wrapper.c and use it to get rid of that file's dependency on magic assembly */

#ifndef __PIC__
#error position-independent code generation is needed to support copyable functions
#endif

// NOTE: to make this more general, it would be nice to be able to specify return type and parameter
// list, but we don't need that here
// NOTE: a copyable function cannot access static variables, because position-independent code uses
// relative offsets to reference those; anything it might need to access has to be passed to it using
// the adjacent_variable located next to the function in it's custom section (and therefore next
// to a copy of the function in a copy of that section)
// this will probably be a pointer-to-a-structure, and in fact our code currently assumes
// that sizeof(adjacent_variable_type) is sizeof(void *)
// NOTE: calling a copyable function at it's original address will have FUNC_ADJ_VAR
// return NULL, and you can't really call any external functions when FUNC_ADJ_VAR is NULL,
// so this should be avoided (also, is the section even executable?)
// NOTE: `;#` serves as a hack to comment out section parameters which would otherwise cause a conflict
// NOTE: alignment to PAGE_SIZE is used to fix an issue where aarch64 used `adrp` and gets an incorrect address in the new location

#define FUNC_ADJ_VAR(func_name) (func_name##_adj_data)

#define COPYABLE_FUNC(func_name)                                                                                                                                                            \
	extern const char __start_##func_name##_section;                                                                                                                                    \
	extern const char __stop_##func_name##_section;                                                                                                                                     \
	static void *func_name##_adj_data __attribute__((no_reorder)) __attribute__((section(#func_name "_section,\"a\";##"))) __attribute__((__used__)) __attribute__((aligned(PAGE_SIZE))) = NULL; \
	void __attribute__((no_reorder)) __attribute__((section(#func_name "_section,\"a\";#"))) __attribute__((__used__)) __attribute__((optimize("O0"))) __attribute__((no_stack_protector)) func_name(void)

void *alloc_executable_memory(size_t size)
{
	void *ptr = mmap(0, size,
			 PROT_READ | PROT_WRITE | PROT_EXEC,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ptr == (void *)-1) {
		perror("mmap");
		return NULL;
	}
	return ptr;
}

typedef void (*funcptr)(void);

#define make_copy_of_function(func, data) _make_copy_of_function(func, &func##_adj_data, data, &__start_##func##_section, &__stop_##func##_section)

funcptr _make_copy_of_function(funcptr func, void *func_adj_data_addr, void *data, const void *func_section_start, const void *func_section_end)
{
	size_t sizeof_func_section = func_section_end - func_section_start;
	size_t func_off = (intptr_t)func - (intptr_t)func_section_start;
	size_t adj_var_off = (intptr_t)func_adj_data_addr - (intptr_t)func_section_start;

	void *testfuncsection_copy = alloc_executable_memory(sizeof_func_section);
	memcpy(testfuncsection_copy, func_section_start, sizeof_func_section);
	funcptr func_copy = (void *)((intptr_t)testfuncsection_copy + func_off);
	void **adj_var_ptr = (void **)((intptr_t)testfuncsection_copy + adj_var_off);

	*adj_var_ptr = data;

	return func_copy;
}

/* --- end of to-move code */

struct stub_func_adj_data {
	int (*printf)(const char *restrict format, ...);
	void (*exit)(int status);
	const char *orig_func_name;
};

COPYABLE_FUNC(symbol_not_linked_stub)
{
	// char fmt_str[] = "..." seems to still use a string literal on aarch64 (anything with fixed width instructions possibly?)
	// the following was used to work around this (first letter ommited since `cat -n` is 1-indexed):
	// echo "BORTING: LINKER_DIE_AT_RUNTIME was set, and someone called a function which we weren't able to link (symbol name: >%s<)" | sed -E "s/(.)/\1\n/g" | cat -n | sed -E "s/ *([^ \t]*)[ \t]*(.)/fmt_str[\1] = '\2';/" | sed "s/'''/\'\\\\''/
	char fmt_str[122];
	fmt_str[0] = 'A';
	fmt_str[1] = 'B';
	fmt_str[2] = 'O';
	fmt_str[3] = 'R';
	fmt_str[4] = 'T';
	fmt_str[5] = 'I';
	fmt_str[6] = 'N';
	fmt_str[7] = 'G';
	fmt_str[8] = ':';
	fmt_str[9] = ' ';
	fmt_str[10] = 'L';
	fmt_str[11] = 'I';
	fmt_str[12] = 'N';
	fmt_str[13] = 'K';
	fmt_str[14] = 'E';
	fmt_str[15] = 'R';
	fmt_str[16] = '_';
	fmt_str[17] = 'D';
	fmt_str[18] = 'I';
	fmt_str[19] = 'E';
	fmt_str[20] = '_';
	fmt_str[21] = 'A';
	fmt_str[22] = 'T';
	fmt_str[23] = '_';
	fmt_str[24] = 'R';
	fmt_str[25] = 'U';
	fmt_str[26] = 'N';
	fmt_str[27] = 'T';
	fmt_str[28] = 'I';
	fmt_str[29] = 'M';
	fmt_str[30] = 'E';
	fmt_str[31] = ' ';
	fmt_str[32] = 'w';
	fmt_str[33] = 'a';
	fmt_str[34] = 's';
	fmt_str[35] = ' ';
	fmt_str[36] = 's';
	fmt_str[37] = 'e';
	fmt_str[38] = 't';
	fmt_str[39] = ',';
	fmt_str[40] = ' ';
	fmt_str[41] = 'a';
	fmt_str[42] = 'n';
	fmt_str[43] = 'd';
	fmt_str[44] = ' ';
	fmt_str[45] = 's';
	fmt_str[46] = 'o';
	fmt_str[47] = 'm';
	fmt_str[48] = 'e';
	fmt_str[49] = 'o';
	fmt_str[50] = 'n';
	fmt_str[51] = 'e';
	fmt_str[52] = ' ';
	fmt_str[53] = 'c';
	fmt_str[54] = 'a';
	fmt_str[55] = 'l';
	fmt_str[56] = 'l';
	fmt_str[57] = 'e';
	fmt_str[58] = 'd';
	fmt_str[59] = ' ';
	fmt_str[60] = 'a';
	fmt_str[61] = ' ';
	fmt_str[62] = 'f';
	fmt_str[63] = 'u';
	fmt_str[64] = 'n';
	fmt_str[65] = 'c';
	fmt_str[66] = 't';
	fmt_str[67] = 'i';
	fmt_str[68] = 'o';
	fmt_str[69] = 'n';
	fmt_str[70] = ' ';
	fmt_str[71] = 'w';
	fmt_str[72] = 'h';
	fmt_str[73] = 'i';
	fmt_str[74] = 'c';
	fmt_str[75] = 'h';
	fmt_str[76] = ' ';
	fmt_str[77] = 'w';
	fmt_str[78] = 'e';
	fmt_str[79] = ' ';
	fmt_str[80] = 'w';
	fmt_str[81] = 'e';
	fmt_str[82] = 'r';
	fmt_str[83] = 'e';
	fmt_str[84] = 'n';
	fmt_str[85] = '\'';
	fmt_str[86] = 't';
	fmt_str[87] = ' ';
	fmt_str[88] = 'a';
	fmt_str[89] = 'b';
	fmt_str[90] = 'l';
	fmt_str[91] = 'e';
	fmt_str[92] = ' ';
	fmt_str[93] = 't';
	fmt_str[94] = 'o';
	fmt_str[95] = ' ';
	fmt_str[96] = 'l';
	fmt_str[97] = 'i';
	fmt_str[98] = 'n';
	fmt_str[99] = 'k';
	fmt_str[100] = ' ';
	fmt_str[101] = '(';
	fmt_str[102] = 's';
	fmt_str[103] = 'y';
	fmt_str[104] = 'm';
	fmt_str[105] = 'b';
	fmt_str[106] = 'o';
	fmt_str[107] = 'l';
	fmt_str[108] = ' ';
	fmt_str[109] = 'n';
	fmt_str[110] = 'a';
	fmt_str[111] = 'm';
	fmt_str[112] = 'e';
	fmt_str[113] = ':';
	fmt_str[114] = ' ';
	fmt_str[115] = '>';
	fmt_str[116] = '%';
	fmt_str[117] = 's';
	fmt_str[118] = '<';
	fmt_str[119] = ')';
	fmt_str[120] = '\n';
	fmt_str[121] = '\0';
	struct stub_func_adj_data *adj_data = FUNC_ADJ_VAR(symbol_not_linked_stub);
	adj_data->printf(fmt_str, adj_data->orig_func_name);
	adj_data->exit(1);
}

static ElfW(Addr) prepare_stub_func(const char* sym_name) {
	struct stub_func_adj_data *data = malloc(sizeof(struct stub_func_adj_data));
	data->printf = &printf;
	data->exit = &exit;
	data->orig_func_name = sym_name;
	return (intptr_t)make_copy_of_function(symbol_not_linked_stub, data);
}

#if defined(USE_RELA)
static int apkenv_reloc_library(soinfo *si, ElfW(Rela) * rela, size_t count)
{
	ElfW(Sym) * s;
	ElfW(Addr) base;

	for (size_t idx = 0; idx < count; ++idx, ++rela) {

		uint32_t type = ELF_R_TYPE(rela->r_info);
		ElfW(Addr) sym = ELF_R_SYM(rela->r_info);

		ElfW(Addr) reloc = (ElfW(Addr))(rela->r_offset + si->base);
		ElfW(Addr) sym_addr = 0;
		const char *sym_name = NULL;
		char wrap_sym_name[1024] = {'b', 'i', 'o', 'n', 'i', 'c', '_'};

		DEBUG("Processing '%s' relocation at index %zd", si->name, idx);

		if (type == 0) { // R_*_NONE
			continue;
		}

		if (sym != 0) {
			sym_name = (const char *)(si->strtab + si->symtab[sym].st_name);

			memcpy(wrap_sym_name + 7, sym_name, MIN(sizeof(wrap_sym_name) - 7, strlen(sym_name)));
			sym_addr = 0;

			if ((sym_addr = (intptr_t)dlsym(RTLD_DEFAULT, wrap_sym_name))) {
				LINKER_DEBUG_PRINTF("%s hooked symbol %s to %016lx\n", si->name, wrap_sym_name, sym_addr);
			} else if ((s = apkenv__do_lookup(si, sym_name, &base))) {
				// normal symbol
			} else if ((sym_addr = (intptr_t)dlsym(RTLD_DEFAULT, sym_name))) {
				if (strstr(sym_name, "pthread_"))
					fprintf(stderr, "symbol may need to be wrapped: %s\n", sym_name);
				LINKER_DEBUG_PRINTF("%s hooked symbol %s to %016lx\n", si->name, sym_name, sym_addr);
			} else if (!sym_addr && !strncmp(sym_name, "gl", 2)) {
				LINKER_DEBUG_PRINTF("=======================================\n");
				LINKER_DEBUG_PRINTF("%s symbol %s is an OpenGL extension?\n", si->name, sym_name);
				if ((sym_addr = (intptr_t)eglGetProcAddress(sym_name)))
					LINKER_DEBUG_PRINTF("%s hooked symbol %s to %016lx\n", si->name, sym_name, sym_addr);
			} else if (!strcmp(sym_name, "sigsetjmp")) {
				// we can't wrap this, so we need to substitute it for the correct function here
				// __sigsetjmp is the glibc version, but the musl version is just sigsetjmp so it should be resolved properly by dslsym
				// and not get here
#ifdef __GLIBC__
				sym_addr = (intptr_t)&__sigsetjmp;
#else
				fprintf(stderr, "sigsetjmp special handling shouldn't be needed on musl\n");
				exit(1);
#endif
			} else {
				// symbol not found
				if (getenv("LINKER_DIE_AT_RUNTIME")) {
					// if this special env is set, and the symbol is a function, link in a stub which only fails when it's actually called
					if (ELF_ST_TYPE(si->symtab[sym].st_info) == STT_FUNC) {
						sym_addr = prepare_stub_func(sym_name);
						fprintf(stderr, "%s hooked symbol %s to symbol_not_linked_stub (LINKER_DIE_AT_RUNTIME)\n", si->name, sym_name);
					}
				}
			}

			if (sym_addr != 0) {
#ifdef __GLIBC__
				Dl_info info;
				ElfW(Sym) * extra;
				if (dladdr1((void *)sym_addr, &info, (void **)&extra, RTLD_DL_SYMENT) && (!extra || ELF_ST_TYPE(extra->st_info) == STT_FUNC))
					sym_addr = (ElfW(Addr))wrapper_create(sym_name, (void *)sym_addr);
#endif
			} else if (s == NULL) {
				// We only allow an undefined symbol if this is a weak reference...
				s = &si->symtab[sym];
				if (ELF_ST_BIND(s->st_info) != STB_WEAK) {
					DL_ERR("cannot locate symbol \"%s\" referenced by \"%s\"...", sym_name, si->name);
					return -1;
				}
				/* IHI0044C AAELF 4.5.1.1:
					 Libraries are not searched to resolve weak references.
					 It is not an error for a weak reference to remain unsatisfied.
					 During linking, the value of an undefined weak reference is:
					 - Zero if the relocation type is absolute
					 - The address of the place if the relocation is pc-relative
					 - The address of nominal base address if the relocation
					 type is base-relative.
				 */
				switch (type) {
#if defined(__aarch64__)
				case R_AARCH64_JUMP_SLOT:
				case R_AARCH64_GLOB_DAT:
				case R_AARCH64_ABS64:
				case R_AARCH64_ABS32:
				case R_AARCH64_ABS16:
				case R_AARCH64_RELATIVE:
					/*
					 * The sym_addr was initialized to be zero above, or the relocation
					 * code below does not care about value of sym_addr.
					 * No need to do anything.
					 */
					break;
#elif defined(__x86_64__)
				case R_X86_64_JUMP_SLOT:
				case R_X86_64_GLOB_DAT:
				case R_X86_64_32:
				case R_X86_64_64:
				case R_X86_64_RELATIVE:
					// No need to do anything.
					break;
				case R_X86_64_PC32:
					sym_addr = reloc;
					break;
#endif
				default:
					DL_ERR("unknown weak reloc type %d @ %p (%zu)", type, rela, idx);
					return -1;
				}
			} else {
				/* We got a definition.  */
				sym_addr = (ElfW(Addr))(s->st_value + base);
				LINKER_DEBUG_PRINTF("%s symbol (from %s) %s to %016lx\n", si->name, apkenv_last_library_used, sym_name, sym_addr);
				if (ELF_ST_TYPE(s->st_info) == STT_FUNC) {
					sym_addr = (ElfW(Addr))wrapper_create(sym_name, (void *)sym_addr);
				}
			}
			COUNT_RELOC(RELOC_SYMBOL);
		} else {
			s = NULL;
		}
		switch (type) {
#if defined(__aarch64__)
		case R_AARCH64_JUMP_SLOT:
			COUNT_RELOC(RELOC_ABSOLUTE);
			MARK(rela->r_offset);
			TRACE_TYPE(RELO, "RELO JMP_SLOT %16llx <- %16llx %s\n",
				   reloc, (sym_addr + rela->r_addend), sym_name);
			*((ElfW(Addr) *)reloc) = (sym_addr + rela->r_addend);
			break;
		case R_AARCH64_GLOB_DAT:
			COUNT_RELOC(RELOC_ABSOLUTE);
			MARK(rela->r_offset);
			TRACE_TYPE(RELO, "RELO GLOB_DAT %16llx <- %16llx %s\n",
				   reloc, (sym_addr + rela->r_addend), sym_name);
			*((ElfW(Addr) *)reloc) = (sym_addr + rela->r_addend);
			break;
		case R_AARCH64_ABS64:
			COUNT_RELOC(RELOC_ABSOLUTE);
			MARK(rela->r_offset);
			TRACE_TYPE(RELO, "RELO ABS64 %16llx <- %16llx %s\n",
				   reloc, (sym_addr + rela->r_addend), sym_name);
			*((ElfW(Addr) *)reloc) += (sym_addr + rela->r_addend);
			break;
		case R_AARCH64_ABS32:
			COUNT_RELOC(RELOC_ABSOLUTE);
			MARK(rela->r_offset);
			TRACE_TYPE(RELO, "RELO ABS32 %16llx <- %16llx %s\n",
				   reloc, (sym_addr + rela->r_addend), sym_name);
			if (((ElfW(Addr))(INT32_MIN) <= (*((ElfW(Addr) *)reloc) + (sym_addr + rela->r_addend))) &&
			    ((*((ElfW(Addr) *)reloc) + (sym_addr + rela->r_addend)) <= (ElfW(Addr))(UINT32_MAX))) {
				*((ElfW(Addr) *)reloc) += (sym_addr + rela->r_addend);
			} else {
				DL_ERR("0x%016llx out of range 0x%016llx to 0x%016llx",
				       (*((ElfW(Addr) *)reloc) + (sym_addr + rela->r_addend)),
				       (ElfW(Addr))(INT32_MIN),
				       (ElfW(Addr))(UINT32_MAX));
				return -1;
			}
			break;
		case R_AARCH64_ABS16:
			COUNT_RELOC(RELOC_ABSOLUTE);
			MARK(rela->r_offset);
			TRACE_TYPE(RELO, "RELO ABS16 %16llx <- %16llx %s\n",
				   reloc, (sym_addr + rela->r_addend), sym_name);
			if (((ElfW(Addr))(INT16_MIN) <= (*((ElfW(Addr) *)reloc) + (sym_addr + rela->r_addend))) &&
			    ((*((ElfW(Addr) *)reloc) + (sym_addr + rela->r_addend)) <= (ElfW(Addr))(UINT16_MAX))) {
				*((ElfW(Addr) *)reloc) += (sym_addr + rela->r_addend);
			} else {
				DL_ERR("0x%016llx out of range 0x%016llx to 0x%016llx",
				       (*((ElfW(Addr) *)reloc) + (sym_addr + rela->r_addend)),
				       (ElfW(Addr))(INT16_MIN),
				       (ElfW(Addr))(UINT16_MAX));
				return -1;
			}
			break;
		case R_AARCH64_PREL64:
			COUNT_RELOC(RELOC_RELATIVE);
			MARK(rela->r_offset);
			TRACE_TYPE(RELO, "RELO REL64 %16llx <- %16llx - %16llx %s\n",
				   reloc, (sym_addr + rela->r_addend), rela->r_offset, sym_name);
			*((ElfW(Addr) *)reloc) += (sym_addr + rela->r_addend) - rela->r_offset;
			break;
		case R_AARCH64_PREL32:
			COUNT_RELOC(RELOC_RELATIVE);
			MARK(rela->r_offset);
			TRACE_TYPE(RELO, "RELO REL32 %16llx <- %16llx - %16llx %s\n",
				   reloc, (sym_addr + rela->r_addend), rela->r_offset, sym_name);
			if (((ElfW(Addr))(INT32_MIN) <= (*((ElfW(Addr) *)reloc) + ((sym_addr + rela->r_addend) - rela->r_offset))) &&
			    ((*((ElfW(Addr) *)reloc) + ((sym_addr + rela->r_addend) - rela->r_offset)) <= (ElfW(Addr))(UINT32_MAX))) {
				*((ElfW(Addr) *)reloc) += ((sym_addr + rela->r_addend) - rela->r_offset);
			} else {
				DL_ERR("0x%016llx out of range 0x%016llx to 0x%016llx",
				       (*((ElfW(Addr) *)reloc) + ((sym_addr + rela->r_addend) - rela->r_offset)),
				       (ElfW(Addr))(INT32_MIN),
				       (ElfW(Addr))(UINT32_MAX));
				return -1;
			}
			break;
		case R_AARCH64_PREL16:
			COUNT_RELOC(RELOC_RELATIVE);
			MARK(rela->r_offset);
			TRACE_TYPE(RELO, "RELO REL16 %16llx <- %16llx - %16llx %s\n",
				   reloc, (sym_addr + rela->r_addend), rela->r_offset, sym_name);
			if (((ElfW(Addr))(INT16_MIN) <= (*((ElfW(Addr) *)reloc) + ((sym_addr + rela->r_addend) - rela->r_offset))) &&
			    ((*((ElfW(Addr) *)reloc) + ((sym_addr + rela->r_addend) - rela->r_offset)) <= (ElfW(Addr))(UINT16_MAX))) {
				*((ElfW(Addr) *)reloc) += ((sym_addr + rela->r_addend) - rela->r_offset);
			} else {
				DL_ERR("0x%016llx out of range 0x%016llx to 0x%016llx",
				       (*((ElfW(Addr) *)reloc) + ((sym_addr + rela->r_addend) - rela->r_offset)),
				       (ElfW(Addr))(INT16_MIN),
				       (ElfW(Addr))(UINT16_MAX));
				return -1;
			}
			break;
		case R_AARCH64_RELATIVE:
			COUNT_RELOC(RELOC_RELATIVE);
			MARK(rela->r_offset);
			if (sym) {
				DL_ERR("odd RELATIVE form...");
				return -1;
			}
			TRACE_TYPE(RELO, "RELO RELATIVE %16llx <- %16llx\n",
				   reloc, (si->base + rela->r_addend));
			*((ElfW(Addr) *)reloc) = (si->base + rela->r_addend);
			break;
		case R_AARCH64_COPY:
			/*
			 * ET_EXEC is not supported so this should not happen.
			 *
			 * http://infocenter.arm.com/help/topic/com.arm.doc.ihi0044d/IHI0044D_aaelf.pdf
			 *
			 * Section 4.7.1.10 "Dynamic relocations"
			 * R_AARCH64_COPY may only appear in executable objects where e_type is
			 * set to ET_EXEC.
			 */
			DL_ERR("%s R_AARCH64_COPY relocations are not supported", si->name);
			return -1;
		case R_AARCH64_TLS_TPREL64:
			TRACE_TYPE(RELO, "RELO TLS_TPREL64 *** %16llx <- %16llx - %16llx\n",
				   reloc, (sym_addr + rela->r_addend), rela->r_offset);
			break;
		case R_AARCH64_TLS_DTPREL32:
			TRACE_TYPE(RELO, "RELO TLS_DTPREL32 *** %16llx <- %16llx - %16llx\n",
				   reloc, (sym_addr + rela->r_addend), rela->r_offset);
			break;
#elif defined(__x86_64__)
		case R_X86_64_JUMP_SLOT:
			COUNT_RELOC(RELOC_ABSOLUTE);
			MARK(rela->r_offset);
			TRACE_TYPE(RELO, "RELO JMP_SLOT %08zx <- %08zx %s\n", (size_t)(reloc),
				   (size_t)(sym_addr + rela->r_addend), sym_name);
			*((ElfW(Addr) *)reloc) = sym_addr + rela->r_addend;
			break;
		case R_X86_64_GLOB_DAT:
			COUNT_RELOC(RELOC_ABSOLUTE);
			MARK(rela->r_offset);

			TRACE_TYPE(RELO, "RELO GLOB_DAT %08zx <- %08zx %s\n", (size_t)(reloc),

				   (size_t)(sym_addr + rela->r_addend), sym_name);
			*((ElfW(Addr) *)reloc) = sym_addr + rela->r_addend;
			break;
		case R_X86_64_RELATIVE:
			COUNT_RELOC(RELOC_RELATIVE);
			MARK(rela->r_offset);
			if (sym) {
				DL_ERR("odd RELATIVE form...");
				return -1;
			}
			TRACE_TYPE(RELO, "RELO RELATIVE %08zx <- +%08zx\n", (size_t)(reloc),
				   (size_t)(si->base));
			*((ElfW(Addr) *)reloc) = si->base + rela->r_addend;
			break;
		case R_X86_64_32:
			COUNT_RELOC(RELOC_RELATIVE);
			MARK(rela->r_offset);
			TRACE_TYPE(RELO, "RELO R_X86_64_32 %08zx <- +%08zx %s\n", (size_t)(reloc),
				   (size_t)(sym_addr), sym_name);
			*((ElfW(Addr) *)reloc) = sym_addr + rela->r_addend;
			break;
		case R_X86_64_64:
			COUNT_RELOC(RELOC_RELATIVE);
			MARK(rela->r_offset);
			TRACE_TYPE(RELO, "RELO R_X86_64_64 %08zx <- +%08zx %s\n", (size_t)(reloc),
				   (size_t)(sym_addr), sym_name);
			*((ElfW(Addr) *)reloc) = sym_addr + rela->r_addend;
			break;
		case R_X86_64_PC32:
			COUNT_RELOC(RELOC_RELATIVE);
			MARK(rela->r_offset);
			TRACE_TYPE(RELO, "RELO R_X86_64_PC32 %08zx <- +%08zx (%08zx - %08zx) %s\n",
				   (size_t)(reloc), (size_t)(sym_addr - reloc),
				   (size_t)(sym_addr), (size_t)(reloc), sym_name);
			*((ElfW(Addr) *)reloc) = sym_addr + rela->r_addend - reloc;
			break;
#endif
		default:
			DL_ERR("unknown reloc type %d @ %p (%zu)", type, rela, idx);
			return -1;
		}
	}
	return 0;
}
#else // REL, not RELA.
static int apkenv_reloc_library(soinfo *si, ElfW(Rel) * rel, size_t count)
{
	ElfW(Sym) *symtab = si->symtab;
	const char *strtab = si->strtab;

	ElfW(Sym) * s;
	ElfW(Addr) base;
	ElfW(Rel) *start = rel;

	for (size_t idx = 0; idx < count; ++idx, ++rel) {

		uint32_t type = ELF_R_TYPE(rel->r_info);
		ElfW(Addr) sym = ELF_R_SYM(rel->r_info);

		ElfW(Addr) reloc = (ElfW(Addr))(rel->r_offset + si->base);
		ElfW(Addr) sym_addr = 0;
		char *sym_name = NULL;
		char wrap_sym_name[1024] = {'b', 'i', 'o', 'n', 'i', 'c', '_'};

		DEBUG("%5d Processing '%s' relocation at index %d\n", apkenv_pid, si->name, idx);

		// TODO: should we have this?
		//		if (type == 0) { // R_*_NONE
		//			continue;
		//		}
		if (sym != 0) {
			sym_name = (const char *)(si->strtab + si->symtab[sym].st_name);

			memcpy(wrap_sym_name + 7, sym_name, MIN(sizeof(wrap_sym_name) - 7, strlen(sym_name)));
			sym_addr = 0;

			if ((sym_addr = (intptr_t)dlsym(RTLD_DEFAULT, wrap_sym_name))) {
				LINKER_DEBUG_PRINTF("%s hooked symbol %s to %x\n", si->name, wrap_sym_name, sym_addr);
			} else if ((s = apkenv__do_lookup(si, sym_name, &base))) {
				// normal symbol
			} else if ((sym_addr = (intptr_t)dlsym(RTLD_DEFAULT, sym_name))) {
				if (strstr(sym_name, "pthread_"))
					fprintf(stderr, "symbol may need to be wrapped: %s\n", sym_name);
				LINKER_DEBUG_PRINTF("%s hooked symbol %s to %x\n", si->name, sym_name, sym_addr);
			} else {
				// symbol not found
				if(getenv("LINKER_DIE_AT_RUNTIME")) {
					// if this special env is set, and the symbol is a function, link in a stub which only fails when it's actually called
					if(ELF_ST_TYPE(si->symtab[sym].st_info) == STT_FUNC) {
						sym_addr = prepare_stub_func(sym_name);
						fprintf(stderr, "%s hooked symbol %s to symbol_not_linked_stub (LINKER_DIE_AT_RUNTIME)\n", si->name, sym_name);
					}
				}
			}

			if (sym_addr != 0) {
#ifdef __GLIBC__
				Dl_info info;
				ElfW(Sym) * extra;
				if (dladdr1((void *)sym_addr, &info, (void **)&extra, RTLD_DL_SYMENT) && (!extra || ELF_ST_TYPE(extra->st_info) == STT_FUNC))
					sym_addr = (ElfW(Addr))wrapper_create(sym_name, (void *)sym_addr);
#endif
			} else if (s == NULL) {
				/* We only allow an undefined symbol if this is a weak
				   reference..   */
				s = &si->symtab[sym];
				if (ELF_ST_BIND(s->st_info) != STB_WEAK) {
					DL_ERR("%5d cannot locate '%s'...\n", apkenv_pid, sym_name);
					return -1;
				}
				/* IHI0044C AAELF 4.5.1.1:
					 Libraries are not searched to resolve weak references.
					 It is not an error for a weak reference to remain unsatisfied.
					 During linking, the value of an undefined weak reference is:
					 - Zero if the relocation type is absolute
					 - The address of the place if the relocation is pc-relative
					 - The address of nominal base address if the relocation
					 type is base-relative.
				 */
				switch (type) {
#if defined(__arm__)
				case R_ARM_JUMP_SLOT:
				case R_ARM_GLOB_DAT:
				case R_ARM_ABS32:
				case R_ARM_RELATIVE: /* Don't care. */
				case R_ARM_NONE:     /* Don't care. */
					break;
#elif defined(__i386__)
				case R_386_JMP_SLOT:
				case R_386_GLOB_DAT:
				case R_386_32:
				case R_386_RELATIVE: /* Dont' care. */
					/* sym_addr was initialized to be zero above or relocation
					   code below does not care about value of sym_addr.
					   No need to do anything.  */
					break;
				case R_386_PC32:
					sym_addr = reloc;
					break;
#endif
#if defined(__arm__)
				case R_ARM_COPY:
					/* Fall through.  Can't really copy if weak symbol is
					   not found in run-time.  */
#endif
				default:
					DL_ERR("%5d unknown weak reloc type %d @ %p (%d)\n",
					       apkenv_pid, type, rel, (int)(rel - start));
					return -1;
				}
			} else {
				/* We got a definition.  */
				sym_addr = (ElfW(Addr))(s->st_value + base);
				LINKER_DEBUG_PRINTF("%s symbol (from %s) %s to %x\n", si->name, apkenv_last_library_used, sym_name, sym_addr);
				if (ELF_ST_TYPE(s->st_info) == STT_FUNC) {
					sym_addr = (ElfW(Addr))wrapper_create(sym_name, (void *)sym_addr);
				}
			}
			COUNT_RELOC(RELOC_SYMBOL);
		} else {
			s = NULL;
		}

		/* TODO: This is ugly. Split up the relocations by arch into
		 * different files.
		 */
		switch (type) {
#if defined(__arm__)
		case R_ARM_JUMP_SLOT:
			COUNT_RELOC(RELOC_ABSOLUTE);
			MARK(rel->r_offset);
			TRACE_TYPE(RELO, "%5d RELO JMP_SLOT %016lx <- %016lx %s\n", apkenv_pid,
				   reloc, sym_addr, sym_name);
			*((ElfW(Addr) *)reloc) = sym_addr;
			break;
		case R_ARM_GLOB_DAT:
			COUNT_RELOC(RELOC_ABSOLUTE);
			MARK(rel->r_offset);

			TRACE_TYPE(RELO, "%5d RELO GLOB_DAT %016lx <- %016lx %s\n", apkenv_pid,
				   reloc, sym_addr, sym_name);

			*((ElfW(Addr) *)reloc) = sym_addr;
			break;
		case R_ARM_ABS32:
			COUNT_RELOC(RELOC_ABSOLUTE);
			MARK(rel->r_offset);
			TRACE_TYPE(RELO, "%5d RELO ABS %016lx <- %016lx %s\n", apkenv_pid,
				   reloc, sym_addr, sym_name);
			*((ElfW(Addr) *)reloc) += sym_addr;
			break;
		case R_ARM_REL32:
			COUNT_RELOC(RELOC_RELATIVE);
			MARK(rel->r_offset);
			TRACE_TYPE(RELO, "%5d RELO REL32 %016lx <- %016lx - %016lx %s\n", apkenv_pid,
				   reloc, sym_addr, rel->r_offset, sym_name);
			*((ElfW(Addr) *)reloc) += sym_addr - rel->r_offset;
			break;
#elif defined(__i386__)
		case R_386_JMP_SLOT:
			COUNT_RELOC(RELOC_ABSOLUTE);
			MARK(rel->r_offset);
			TRACE_TYPE(RELO, "%5d RELO JMP_SLOT %016lx <- %016lx %s\n", apkenv_pid,
				   reloc, sym_addr, sym_name);
			*((ElfW(Addr) *)reloc) = sym_addr;
			break;
		case R_386_GLOB_DAT:
			COUNT_RELOC(RELOC_ABSOLUTE);
			MARK(rel->r_offset);
			TRACE_TYPE(RELO, "%5d RELO GLOB_DAT %016lx <- %016lx %s\n", apkenv_pid,
				   reloc, sym_addr, sym_name);
			*((ElfW(Addr) *)reloc) = sym_addr;
			break;
#endif

#if defined(__arm__)
		case R_ARM_RELATIVE:
#elif defined(__i386__)
		case R_386_RELATIVE:
#endif
			COUNT_RELOC(RELOC_RELATIVE);
			MARK(rel->r_offset);
			if (sym) {
				DL_ERR("%5d odd RELATIVE form...", apkenv_pid);
				return -1;
			}
			TRACE_TYPE(RELO, "%5d RELO RELATIVE %016lx <- +%016lx\n", apkenv_pid,
				   reloc, si->base);
			*((ElfW(Addr) *)reloc) += si->base;
			break;

#if defined(__i386__)
		case R_386_32:
			COUNT_RELOC(RELOC_RELATIVE);
			MARK(rel->r_offset);

			TRACE_TYPE(RELO, "%5d RELO R_386_32 %016lx <- +%016lx %s\n", apkenv_pid,
				   reloc, sym_addr, sym_name);
			*((ElfW(Addr) *)reloc) += (uint32_t)sym_addr;
			break;

		case R_386_PC32:
			COUNT_RELOC(RELOC_RELATIVE);
			MARK(rel->r_offset);
			TRACE_TYPE(RELO, "%5d RELO R_386_PC32 %016lx <- "
					 "+%016lx (%016lx - %016lx) %s\n",
				   apkenv_pid, reloc,
				   (sym_addr - reloc), sym_addr, reloc, sym_name);
			*((ElfW(Addr) *)reloc) += (uint32_t)(sym_addr - reloc);
			break;
#endif

#ifdef __arm__
		case R_ARM_COPY:
			COUNT_RELOC(RELOC_COPY);
			MARK(rel->r_offset);
			TRACE_TYPE(RELO, "%5d RELO %016lx <- %d @ %016lx %s\n", apkenv_pid,
				   reloc, s->st_size, sym_addr, sym_name);
			memcpy((void *)reloc, (void *)sym_addr, s->st_size);
			break;
		case R_ARM_NONE:
			break;
#endif

		default:
			DL_ERR("%5d unknown reloc type %d @ %p (%d)",
			       apkenv_pid, type, rel, (int)(rel - start));
			return -1;
		}
	}
	return 0;
}
#endif

void apkenv_apply_relr_reloc(soinfo *si, ElfW(Addr) offset) {
	ElfW(Addr) address = offset + si->base;
	*((ElfW(Addr)*)address) += si->base;
}

static bool apkenv_relocate_relr(soinfo *si) {
	ElfW(Relr)* begin = si->relr_;
	ElfW(Relr)* end = si->relr_ + si->relr_count_;

	const size_t wordsize = sizeof(ElfW(Addr));

	ElfW(Addr) base = 0;
	for (ElfW(Relr)* current = begin; current < end; ++current) {
		ElfW(Relr) entry = *current;
		ElfW(Addr) offset;
		if ((entry&1) == 0) {
			// Even entry: encodes the offset for next relocation.
			offset = (ElfW(Addr))entry;
			apkenv_apply_relr_reloc(si, offset);
			// Set base offset for subsequent bitmap entries.
			base = offset + wordsize;
			continue;
		}
		// Odd entry: encodes bitmap for relocations starting at base.
		offset = base;
		while (entry != 0) {
			entry >>= 1;
			if ((entry&1) != 0) {
				apkenv_apply_relr_reloc(si, offset);
			}
			offset += wordsize;
		}
		// Advance base offset by 63 words for 64-bit platforms,
		// or 31 words for 32-bit platforms.
		base += (8*wordsize - 1) * wordsize;
	}
	return true;
}

/* Please read the "Initialization and Termination functions" functions.
 * of the linker design note in bionic/linker/README.TXT to understand
 * what the following code is doing.
 *
 * The important things to remember are:
 *
 *   DT_PREINIT_ARRAY must be called first for executables, and should
 *   not appear in shared libraries.
 *
 *   DT_INIT should be called before DT_INIT_ARRAY if both are present
 *
 *   DT_FINI should be called after DT_FINI_ARRAY if both are present
 *
 *   DT_FINI_ARRAY must be parsed in reverse order.
 */

static void apkenv_call_array(intptr_t *ctor, int count, int reverse)
{
	int n, inc = 1;

	if (reverse) {
		ctor += (count - 1);
		inc = -1;
	}

	for (n = count; n > 0; n--) {
		TRACE("[ %5d Looking at %s *0x%016lx == 0x%016lx ]\n", apkenv_pid,
		      reverse ? "dtor" : "ctor",
		      (intptr_t)ctor, (intptr_t)*ctor);
		void (*func)() = (void (*)()) * ctor;
		ctor += inc;
		if (((intptr_t)func == 0) || ((intptr_t)func == -1))
			continue;
		TRACE("[ %5d Calling func @ 0x%016lx ]\n", apkenv_pid, (intptr_t)func);
		func();
	}
}

void apkenv_call_constructors_recursive(soinfo *si)
{
	if (si->constructors_called)
		return;

	// Set this before actually calling the constructors, otherwise it doesn't
	// protect against recursive constructor calls. One simple example of
	// constructor recursion is the libc debug malloc, which is implemented in
	// libc_malloc_debug_leak.so:
	// 1. The program depends on libc, so libc's constructor is called here.
	// 2. The libc constructor calls dlopen() to load libc_malloc_debug_leak.so.
	// 3. dlopen() calls apkenv_call_constructors_recursive() with the newly created
	//    soinfo for libc_malloc_debug_leak.so.
	// 4. The debug so depends on libc, so apkenv_call_constructors_recursive() is
	//    called again with the libc soinfo. If it doesn't trigger the early-
	//    out above, the libc constructor will be called again (recursively!).
	si->constructors_called = 1;

	TRACE("[ %5d Calling preinit_array @ 0x%016lx [%lu] for '%s' ]\n",
	      apkenv_pid, (intptr_t)si->preinit_array, si->preinit_array_count,
	      si->name);
	apkenv_call_array(si->preinit_array, si->preinit_array_count, 0);
	TRACE("[ %5d Done calling preinit_array for '%s' ]\n", apkenv_pid, si->name);

	if (si->dynamic) {
		for (ElfW(Dyn) *d = si->dynamic; d->d_tag != DT_NULL; d++) {
			if (d->d_tag == DT_NEEDED) {
				soinfo *lsi = (soinfo *)d->d_un.d_val;
				if (!apkenv_validate_soinfo(lsi)) {
					DL_ERR("%5d bad DT_NEEDED pointer in %s",
					       apkenv_pid, si->name);
				} else {
					apkenv_call_constructors_recursive(lsi);
				}
			}
		}
	}

	if (si->init_func) {
		TRACE("[ %5d Calling init_func @ 0x%016lx for '%s' ]\n", apkenv_pid,
		      (intptr_t)si->init_func, si->name);
		si->init_func();
		TRACE("[ %5d Done calling init_func for '%s' ]\n", apkenv_pid, si->name);
	}

	if (si->init_array) {
		TRACE("[ %5d Calling init_array @ 0x%016lx [%lu] for '%s' ]\n", apkenv_pid,
		      (intptr_t)si->init_array, si->init_array_count, si->name);
		apkenv_call_array(si->init_array, si->init_array_count, 0);
		TRACE("[ %5d Done calling init_array for '%s' ]\n", apkenv_pid, si->name);
	}
}

static void apkenv_call_destructors(soinfo *si)
{
	if (si->fini_array) {
		TRACE("[ %5d Calling fini_array @ 0x%016lx [%lu] for '%s' ]\n", apkenv_pid,
		      (intptr_t)si->fini_array, si->fini_array_count, si->name);
		apkenv_call_array(si->fini_array, si->fini_array_count, 1);
		TRACE("[ %5d Done calling fini_array for '%s' ]\n", apkenv_pid, si->name);
	}

	if (si->fini_func) {
		TRACE("[ %5d Calling fini_func @ 0x%016lx for '%s' ]\n", apkenv_pid,
		      (intptr_t)si->fini_func, si->name);
		si->fini_func();
		TRACE("[ %5d Done calling fini_func for '%s' ]\n", apkenv_pid, si->name);
	}
}

/* Force any of the closed stdin, stdout and stderr to be associated with
   /dev/null. */
static int apkenv_nullify_closed_stdio(void)
{
	int dev_null, i, status;
	int return_value = 0;

	dev_null = open("/dev/null", O_RDWR);
	if (dev_null < 0) {
		DL_ERR("Cannot open /dev/null.");
		return -1;
	}
	TRACE("[ %5d Opened /dev/null file-descriptor=%d]\n", apkenv_pid, dev_null);

	/* If any of the stdio file descriptors is valid and not associated
	   with /dev/null, dup /dev/null to it.  */
	for (i = 0; i < 3; i++) {
		/* If it is /dev/null already, we are done. */
		if (i == dev_null)
			continue;

		TRACE("[ %5d Nullifying stdio file descriptor %d]\n", apkenv_pid, i);
		/* The man page of fcntl does not say that fcntl(..,F_GETFL)
		   can be interrupted but we do this just to be safe. */
		do {
			status = fcntl(i, F_GETFL);
		} while (status < 0 && errno == EINTR);

		/* If file is openned, we are good. */
		if (status >= 0)
			continue;

		/* The only error we allow is that the file descriptor does not
		   exist, in which case we dup /dev/null to it. */
		if (errno != EBADF) {
			DL_ERR("nullify_stdio: unhandled error %s", strerror(errno));
			return_value = -1;
			continue;
		}

		/* Try dupping /dev/null to this stdio file descriptor and
		   repeat if there is a signal.  Note that any errors in closing
		   the stdio descriptor are lost.  */
		do {
			status = dup2(dev_null, i);
		} while (status < 0 && errno == EINTR);

		if (status < 0) {
			DL_ERR("nullify_stdio: dup2 error %s", strerror(errno));
			return_value = -1;
			continue;
		}
	}

	/* If /dev/null is not one of the stdio file descriptors, close it. */
	if (dev_null > 2) {
		TRACE("[ %5d Closing /dev/null file-descriptor=%d]\n", apkenv_pid, dev_null);
		do {
			status = close(dev_null);
		} while (status < 0 && errno == EINTR);

		if (status < 0) {
			DL_ERR("nullify_stdio: close error %s", strerror(errno));
			return_value = -1;
		}
	}

	return return_value;
}

static void apkenv_wrap_function(void *sym_addr, char *sym_name, int is_thumb, soinfo *si)
{
#ifdef APKENV_LATEHOOKS
	void *hook = NULL;
	if ((hook = apkenv_get_hooked_symbol(sym_name, 0)) != NULL) {
		// if we have a hook redirect the call to that by overwriting
		// the first 2 instruction of the function
		// this should work in any case unless the hook is wrong
		// or the function shorter than 64-bit (2 32-Bit instructions)
		if (!is_thumb) {
			DEBUG("HOOKING INTERNAL (ARM) FUNCTION %s@%x (in %s) TO: %x\n", sym_name, sym_addr, si->name, hook);
			((int32_t *)sym_addr)[0] = 0xe51ff004; // ldr pc, [pc, -#4] (load the hooks address into pc)
			((int32_t *)sym_addr)[1] = (uint32_t)wrapper_create(sym_name, hook);

			__clear_cache((int32_t *)sym_addr, (int32_t *)sym_addr + 2);
		} else {
			DEBUG("HOOKING INTERNAL (THUMB) FUNCTION %s@%x (in %s) TO: %x\n", sym_name, sym_addr, si->name, hook);
			sym_addr = (void *)((char *)sym_addr - 1); // get actual sym_addr

			((int16_t *)sym_addr)[0] = 0xB401; /* push {r0} */
			((int16_t *)sym_addr)[1] = 0xF8DF; /* ldr r0, [pc, #8] */
			((int16_t *)sym_addr)[2] = 0x0008; /* continuation of last instruction */
			((int16_t *)sym_addr)[3] = 0x4684; /* mov ip, r0 */
			((int16_t *)sym_addr)[4] = 0xBC01; /* pop {r0} */
			((int16_t *)sym_addr)[5] = 0x4760; /* bx ip */

			void *wrp = wrapper_create(sym_name, hook);

			// store the hooks address
			((int16_t *)sym_addr)[6] = (uint32_t)wrp & 0x0000FFFF;
			((int16_t *)sym_addr)[7] = (uint32_t)wrp >> 16;

			__clear_cache((int16_t *)sym_addr, (int16_t *)sym_addr + 8);
		}
	} else
#endif /* APKENV_LATEHOOKS */
		// TODO: this will fail if the first 2 instructions do something pc related
		// (this DOES NOT happen very often)
		if (sym_addr && !is_thumb) {
			DEBUG("CREATING ARM WRAPPER FOR: %s@%p (in %s)\n", sym_name, sym_addr, si->name);

			wrapper_create(sym_name, sym_addr);
		}
		// TODO: this will fail if the first 2-5 instructions do something pc related
		// (this DOES happen very often)
		else if (sym_addr && is_thumb) {
			DEBUG("CREATING THUMB WRAPPER FOR: %s@%p (in %s)\n", sym_name, sym_addr, si->name);

			wrapper_create(sym_name, sym_addr);
		}
}

/* Franz-Josef Haider apkenv_create_latehook_wrappers */
static void apkenv_create_latehook_wrappers(soinfo *si)
{
	ElfW(Sym) * s;

	unsigned int n = 0;
	for (n = 0; n < si->nchain; n++) {
		s = si->symtab + n;
		switch (ELF_ST_BIND(s->st_info)) {
		case STB_GLOBAL:
		case STB_WEAK:
			if (s->st_shndx == 0)
				continue;

			if (ELF_ST_TYPE(s->st_info) == STT_FUNC) // only wrap functions
			{
				char *sym_name = (char *)(si->strtab + s->st_name);
				void *sym_addr = (void *)(intptr_t)(si->base + s->st_value);

				int is_thumb = ((int32_t)(intptr_t)sym_addr) & 0x00000001;

				apkenv_wrap_function(sym_addr, sym_name, is_thumb, si);
			}
		}
	}
}

static int apkenv_link_image(soinfo *si, /*unused...?*/ unsigned wr_offset)
{
	ElfW(Phdr) *phdr = si->phdr;
	int phnum = si->phnum;

	INFO("[ %5d linking %s ]\n", apkenv_pid, si->name);
	DEBUG("%5d si->base = 0x%016lx si->flags = 0x%08x\n", apkenv_pid,
	      si->base, si->flags);

	if (si->flags & (FLAG_EXE | FLAG_LINKER)) {
		/* Locate the needed program segments (DYNAMIC/ARM_EXIDX) for
		 * linkage info if this is the executable or the linker itself.
		 * If this was a dynamic lib, that would have been done at load time.
		 *
		 * TODO: It's unfortunate that small pieces of this are
		 * repeated from the apkenv_load_library routine. Refactor this just
		 * slightly to reuse these bits.
		 */
		si->size = 0;
		for (; phnum > 0; --phnum, ++phdr) {
#if defined(__arm__)
			if (phdr->p_type == PT_ARM_EXIDX) {
				/* exidx entries (used for stack unwinding) are 8 bytes each.
				 */
				si->ARM_exidx = (ElfW(Addr) *)phdr->p_vaddr;
				si->ARM_exidx_count = phdr->p_memsz / 8;
			}
#endif
			if (phdr->p_type == PT_LOAD) {
				/* For the executable, we use the si->size field only in
				   dl_unwind_find_exidx(), so the meaning of si->size
				   is not the size of the executable; it is the distance
				   between the load location of the executable and the last
				   address of the loadable part of the executable.
				   We use the range [si->base, si->base + si->size) to
				   determine whether a PC value falls within the executable
				   section. Of course, if a value is between si->base and
				   (si->base + phdr->p_vaddr), it's not in the executable
				   section, but a) we shouldn't be asking for such a value
				   anyway, and b) if we have to provide an EXIDX for such a
				   value, then the executable's EXIDX is probably the better
				   choice.
				*/
				DEBUG_DUMP_PHDR(phdr, "PT_LOAD", apkenv_pid);
				if (phdr->p_vaddr + phdr->p_memsz > si->size)
					si->size = phdr->p_vaddr + phdr->p_memsz;
				/* try to remember what range of addresses should be write
				 * protected */
				if (!(phdr->p_flags & PF_W)) {
					intptr_t _end;

					if (si->base + phdr->p_vaddr < si->wrprotect_start)
						si->wrprotect_start = si->base + phdr->p_vaddr;
					_end = (((si->base + phdr->p_vaddr + phdr->p_memsz + PAGE_SIZE - 1) &
						 (~PAGE_MASK)));
					if (_end > si->wrprotect_end)
						si->wrprotect_end = _end;
					/* Make the section writable just in case we'll have to
					 * write to it during relocation (i.e. text segment).
					 * However, we will remember what range of addresses
					 * should be write protected.
					 */
					mprotect((void *)(si->base + phdr->p_vaddr),
						 phdr->p_memsz,
						 PFLAGS_TO_PROT(phdr->p_flags) | PROT_WRITE);
				}
			} else if (phdr->p_type == PT_DYNAMIC) {
				if (si->dynamic != (ElfW(Dyn) *)-1) {
					DL_ERR("%5d multiple PT_DYNAMIC segments found in '%s'. "
					       "Segment at 0x%016lx, previously one found at 0x%016lx",
					       apkenv_pid, si->name, si->base + phdr->p_vaddr,
					       (intptr_t)si->dynamic);
					goto fail;
				}
				DEBUG_DUMP_PHDR(phdr, "PT_DYNAMIC", apkenv_pid);
				si->dynamic = (ElfW(Dyn) *)(si->base + phdr->p_vaddr);
			} else if (phdr->p_type == PT_GNU_RELRO) {
				if ((phdr->p_vaddr >= si->size) || ((phdr->p_vaddr + phdr->p_memsz) > si->size) || ((si->base + phdr->p_vaddr + phdr->p_memsz) < si->base)) {
					DL_ERR("%d invalid GNU_RELRO in '%s' "
					       "p_vaddr=0x%016lx p_memsz=0x%016lx",
					       apkenv_pid, si->name,
					       phdr->p_vaddr, phdr->p_memsz);
					goto fail;
				}
				si->gnu_relro_start = (ElfW(Addr))(si->base + phdr->p_vaddr);
				si->gnu_relro_len = (size_t)phdr->p_memsz;
			}
		}
	}

	if (si->dynamic == (ElfW(Dyn) *)-1) {
		DL_ERR("%5d missing PT_DYNAMIC?!", apkenv_pid);
		goto fail;
	}

	DEBUG("%5d dynamic = %p\n", apkenv_pid, si->dynamic);

	/* extract useful information from dynamic section */
	for (ElfW(Dyn) *d = si->dynamic; d->d_tag != DT_NULL; d++) {
		DEBUG("%5d d = %p, d->d_tag = 0x%016lx d->d_un.d_val = 0x%016lx\n", apkenv_pid, d, d->d_tag, d->d_un.d_val);
		switch (d->d_tag) {
		case DT_HASH:
			if (si->nbucket != 0) {
				// in case of --hash-style=both, we prefer gnu
				break;
			}

			si->nbucket = ((uint32_t *)(si->base + d->d_un.d_ptr))[0];
			si->nchain = ((uint32_t *)(si->base + d->d_un.d_ptr))[1];
			si->bucket = (uint32_t *)(si->base + d->d_un.d_ptr + 8);
			si->chain = (uint32_t *)(si->base + d->d_un.d_ptr + 8 + si->nbucket * 4);
			break;
		case DT_GNU_HASH:
			if (si->nbucket != 0) {
				// in case of --hash-style=both, we prefer gnu
				si->nchain = 0;
			}

			si->nbucket = ((uint32_t *)(si->base + d->d_un.d_ptr))[0];
			// skip symndx
			si->gnu_maskwords = ((uint32_t *)(si->base + d->d_un.d_ptr))[2];
			si->gnu_shift2 = ((uint32_t *)(si->base + d->d_un.d_ptr))[3];

			si->gnu_bloom_filter = (ElfW(Addr) *)(si->base + d->d_un.d_ptr + 16);
			si->bucket = (uint32_t *)(si->gnu_bloom_filter + si->gnu_maskwords);
			// amend chain for symndx = header[1]
			si->chain = si->bucket + si->nbucket - ((uint32_t *)(si->base + d->d_un.d_ptr))[1];

			if (!powerof2(si->gnu_maskwords)) {
				DL_ERR("invalid maskwords for gnu_hash = 0x%x, in \"%s\" expecting power to two", si->gnu_maskwords, si->name);
				return false;
			}
			si->gnu_maskwords--;

			si->flags |= FLAG_GNU_HASH;
			break;
		case DT_STRTAB:
			si->strtab = (const char *)(si->base + d->d_un.d_ptr);
			break;
		case DT_SYMTAB:
			si->symtab = (ElfW(Sym) *)(si->base + d->d_un.d_ptr);
			break;
#if !defined(__LP64__)
		case DT_PLTREL:
			if (d->d_un.d_val != DT_REL) {
				DL_ERR("DT_RELA not supported");
				goto fail;
			}
			break;
#endif
		case DT_JMPREL:
#if defined(USE_RELA)
			si->plt_rela = (ElfW(Rela) *)(si->base + d->d_un.d_ptr);
#else
			si->plt_rel = (ElfW(Rel) *)(si->base + d->d_un.d_ptr);
#endif
			break;
		case DT_PLTRELSZ:
#if defined(USE_RELA)
			si->plt_rela_count = d->d_un.d_val / sizeof(ElfW(Rela));
#else
			si->plt_rel_count = d->d_un.d_val / sizeof(ElfW(Rel));
#endif
			break;
		case DT_PLTGOT:
			/* Save this in case we decide to do lazy binding. We don't yet. */
			si->plt_got = (ElfW(Addr) **)(si->base + d->d_un.d_ptr);
			break;
		case DT_DEBUG:
			// Set the DT_DEBUG entry to the addres of _r_debug for GDB
			d->d_un.d_val = (uintptr_t)_r_debug_ptr;
			break;
#if defined(USE_RELA)
		case DT_RELA:
			si->rela = (ElfW(Rela) *)(si->base + d->d_un.d_ptr);
			break;
		case DT_RELASZ:
			si->rela_count = d->d_un.d_val / sizeof(ElfW(Rela));
			break;
		case DT_REL:
			DL_ERR("unsupported DT_REL in \"%s\"", si->name);
			return false;
		case DT_RELSZ:
			DL_ERR("unsupported DT_RELSZ in \"%s\"", si->name);
			return false;
#else
		case DT_REL:
			si->rel = (ElfW(Rel) *)(si->base + d->d_un.d_ptr);
			break;
		case DT_RELSZ:
			si->rel_count = d->d_un.d_val / sizeof(ElfW(Rel));
			break;
		case DT_RELA:
			DL_ERR("unsupported DT_RELA in \"%s\"", si->name);
			return false;
#endif
		case DT_RELR:
		case DT_ANDROID_RELR:
			si->relr_ = (ElfW(Relr) *)(si->base + d->d_un.d_ptr);
			break;
		case DT_RELRSZ:
		case DT_ANDROID_RELRSZ:
			si->relr_count_ = d->d_un.d_val / sizeof(ElfW(Relr));
			break;
		case DT_INIT:
			si->init_func = (void (*)(void))(si->base + d->d_un.d_ptr);
			DEBUG("%5d %s constructors (init func) found at %p\n",
			      apkenv_pid, si->name, si->init_func);
			break;
		case DT_FINI:
			si->fini_func = (void (*)(void))(si->base + d->d_un.d_ptr);
			DEBUG("%5d %s destructors (fini func) found at %p\n",
			      apkenv_pid, si->name, si->fini_func);
			break;
		case DT_INIT_ARRAY:
			si->init_array = (intptr_t *)(si->base + d->d_un.d_ptr);
			DEBUG("%5d %s constructors (init_array) found at %p\n",
			      apkenv_pid, si->name, si->init_array);
			break;
		case DT_INIT_ARRAYSZ:
			si->init_array_count = ((size_t)d->d_un.d_val) / sizeof(ElfW(Addr));
			break;
		case DT_FINI_ARRAY:
			si->fini_array = (intptr_t *)(si->base + d->d_un.d_ptr);
			DEBUG("%5d %s destructors (fini_array) found at %p\n",
			      apkenv_pid, si->name, si->fini_array);
			break;
		case DT_FINI_ARRAYSZ:
			si->fini_array_count = ((size_t)d->d_un.d_val) / sizeof(ElfW(Addr));
			break;
		case DT_PREINIT_ARRAY:
			si->preinit_array = (intptr_t *)(si->base + d->d_un.d_ptr);
			DEBUG("%5d %s constructors (preinit_array) found at %p\n",
			      apkenv_pid, si->name, si->preinit_array);
			break;
		case DT_PREINIT_ARRAYSZ:
			si->preinit_array_count = ((size_t)d->d_un.d_val) / sizeof(ElfW(Addr));
			break;
		case DT_TEXTREL:
			/* TODO: make use of this. */
			/* this means that we might have to write into where the text
			 * segment was loaded during relocation... Do something with
			 * it.
			 */
			DEBUG("%5d Text segment should be writable during relocation.\n",
			      apkenv_pid);
			break;
		}
	}

	DEBUG("%5d si->base = 0x%016lx, si->strtab = %p, si->symtab = %p\n",
	      apkenv_pid, si->base, si->strtab, si->symtab);

	if ((si->strtab == 0) || (si->symtab == 0)) {
		DL_ERR("%5d missing essential tables", apkenv_pid);
		goto fail;
	}

	/* if this is the main executable, then load all of the apkenv_preloads now */
	if (si->flags & FLAG_EXE) {
		int i;
		memset(apkenv_preloads, 0, sizeof(apkenv_preloads));
		for (i = 0; apkenv_ldpreload_names[i] != NULL; i++) {
			soinfo *lsi = apkenv_find_library(apkenv_ldpreload_names[i], true, RTLD_NOW, NULL);
			if (lsi == 0) {
				apkenv_strlcpy(apkenv_tmp_err_buf, apkenv_linker_get_error(), sizeof(apkenv_tmp_err_buf));
// not sure if this is "fixable", but worst case scenario is truncated error message, so silencing it
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
				DL_ERR("%5d could not load needed library '%s' for '%s' (%s)",
				       apkenv_pid, apkenv_ldpreload_names[i], si->name, apkenv_tmp_err_buf);
#pragma GCC diagnostic pop
				goto fail;
			}
			lsi->refcount++;
			apkenv_preloads[i] = lsi;
		}
	}

	for (ElfW(Dyn) *d = si->dynamic; d->d_tag != DT_NULL; d++) {
		if (d->d_tag == DT_NEEDED) {
			DEBUG("%5d %s needs %s\n", apkenv_pid, si->name, si->strtab + d->d_un.d_val);
			soinfo *lsi = NULL;
			// if (get_builtin_lib_handle(si->strtab + d->d_un.d_val) == NULL)
			lsi = apkenv_find_library(si->strtab + d->d_un.d_val, true, RTLD_NOW, NULL);
			if (lsi == 0) {
				/**
				 * XXX Dirty Hack Alarm --thp XXX
				 *
				 * For libraries that are not found, simply use libdl's soinfo,
				 * in order to not break downstream users of the library while
				 * still allowing the application to start up.
				 **/
				lsi = &apkenv_libdl_info;
				// apkenv_strlcpy(apkenv_tmp_err_buf, apkenv_linker_get_error(), sizeof(apkenv_tmp_err_buf));
				// DL_ERR("%5d could not load needed library '%s' for '%s' (%s)",
				//        apkenv_pid, si->strtab + d->d_un.d_val, si->name, apkenv_tmp_err_buf);
				// continue;
				// goto fail;
			}
			/* Save the soinfo of the loaded DT_NEEDED library in the payload
			   of the DT_NEEDED entry itself, so that we can retrieve the
			   soinfo directly later from the dynamic segment.  This is a hack,
			   but it allows us to map from DT_NEEDED to soinfo efficiently
			   later on when we resolve relocations, trying to look up a symbol
			   with dlsym().
			*/
			d->d_un.d_val = (intptr_t)lsi;
			lsi->refcount++;
		}
	}

#if defined(USE_RELA)
	if (si->plt_rela != NULL) {
		DEBUG("[ %5d relocating %s plt ]\n", apkenv_pid, si->name);
		if (apkenv_reloc_library(si, si->plt_rela, si->plt_rela_count))
			goto fail;
	}

	if (si->rela != NULL) {
		DEBUG("[ %5d relocating %s ]\n", apkenv_pid, si->name);
		if (apkenv_reloc_library(si, si->rela, si->rela_count))
			goto fail;
	}
#else
	if (si->plt_rel) {
		DEBUG("[ %5d relocating %s plt ]\n", apkenv_pid, si->name);
		if (apkenv_reloc_library(si, si->plt_rel, si->plt_rel_count))
			goto fail;
	}
	if (si->rel) {
		DEBUG("[ %5d relocating %s ]\n", apkenv_pid, si->name);
		if (apkenv_reloc_library(si, si->rel, si->rel_count))
			goto fail;
	}
#endif

	if (si->relr_) {
		DEBUG("[ %5d relocating %s relr ]\n", apkenv_pid, si->name);
		if (!apkenv_relocate_relr(si))
			goto fail;
	}

	apkenv_create_latehook_wrappers(si);

	si->flags |= FLAG_LINKED;
	DEBUG("[ %5d finished linking %s ]\n", apkenv_pid, si->name);

#if 0
	/* This is the way that the old dynamic linker did protection of
	* non-writable areas. It would scan section headers and find where
	* .text ended (rather where .data/.bss began) and assume that this is
	* the upper range of the non-writable area. This is too coarse,
	* and is kept here for reference until we fully move away from single
	* segment elf objects. See the code in get_wr_offset (also #if'd 0)
	* that made this possible.
	*/
	if(wr_offset < 0xffffffff){
	mprotect((void*) si->base, wr_offset, PROT_READ | PROT_EXEC);
}
#else
	/* TODO: Verify that this does the right thing in all cases, as it
	 * presently probably does not. It is possible that an ELF image will
	 * come with multiple read-only segments. What we ought to do is scan
	 * the program headers again and mprotect all the read-only segments.
	 * To prevent re-scanning the program header, we would have to build a
	 * list of loadable segments in si, and then scan that instead. */
	if (si->wrprotect_start != 0xffffffff && si->wrprotect_end != 0) {
		mprotect((void *)(intptr_t)si->wrprotect_start,
			 si->wrprotect_end - si->wrprotect_start,
			 PROT_READ | PROT_EXEC);
	}
#endif

	if (si->gnu_relro_start != 0 && si->gnu_relro_len != 0) {
		ElfW(Addr) start = (si->gnu_relro_start & ~PAGE_MASK);
		size_t len = (si->gnu_relro_start - start) + si->gnu_relro_len;
		if (mprotect((void *)(intptr_t)start, len, PROT_READ) < 0) {
			DL_ERR("%5d GNU_RELRO mprotect of library '%s' failed: %d (%s)\n",
			       apkenv_pid, si->name, errno, strerror(errno));
			goto fail;
		}
	}

	/* If this is a SET?ID program, dup /dev/null to opened stdin,
	   stdout and stderr to close a security hole described in:

	ftp://ftp.freebsd.org/pub/FreeBSD/CERT/advisories/FreeBSD-SA-02:23.stdio.asc

	 */
	if (apkenv_program_is_setuid)
		apkenv_nullify_closed_stdio();
	apkenv_notify_gdb_of_load(si);
	return 0;

fail:
	ERROR("failed to link %s\n", si->name);
	si->flags |= FLAG_ERROR;
	return -1;
}

void dl_parse_library_path(const char *path, char *delim)
{
	size_t len;
	char *apkenv_ldpaths_bufp = apkenv_ldpaths_buf;
	int i = 0;

	len = apkenv_strlcpy(apkenv_ldpaths_buf, path, sizeof(apkenv_ldpaths_buf));

	while (i < LDPATH_MAX && (apkenv_ldpaths[i] = strsep(&apkenv_ldpaths_bufp, delim))) {
		if (*apkenv_ldpaths[i] != '\0')
			++i;
	}

	/* Forget the last path if we had to truncate; this occurs if the 2nd to
	 * last char isn't '\0' (i.e. not originally a delim). */
	if (i > 0 && len >= sizeof(apkenv_ldpaths_buf) &&
	    apkenv_ldpaths_buf[sizeof(apkenv_ldpaths_buf) - 2] != '\0') {
		apkenv_ldpaths[i - 1] = NULL;
	} else {
		apkenv_ldpaths[i] = NULL;
	}
}
