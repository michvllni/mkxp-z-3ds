/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minimal <sys/mman.h> for CRuby on the Nintendo 3DS (devkitARM/newlib has none).
 * Only anonymous private mappings are supported (backed by memalign, see rb3ds_shim.c);
 * file mappings fail with ENODEV. mprotect is a no-op: no guard pages on the 3DS.
 */
#ifndef RB3DS_SYS_MMAN_H
#define RB3DS_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void *)-1)

#define MADV_NORMAL   0
#define MADV_DONTNEED 4
#define MADV_FREE     8

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t length, int prot);
int madvise(void *addr, size_t length, int advice);

#endif
