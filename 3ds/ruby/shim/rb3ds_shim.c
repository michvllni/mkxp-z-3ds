// SPDX-License-Identifier: GPL-2.0-or-later
// POSIX functions CRuby 3.1 needs that devkitARM's newlib and libctru do not provide.
// Semantics are the minimum CRuby relies on; see 3ds/ruby/README.md.
#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "rb3ds_compat.h"
#include "rb3ds.h"

size_t rb3ds_main_stack_size = 0;

#define RB3DS_PAGE_SIZE 4096

// Anonymous private mappings only, page-aligned and zero-filled like the kernel would.
// munmap must get the start address of a mapping (CRuby only unmaps whole mappings).
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
	(void)addr;
	(void)prot;
	(void)offset;
	if (!(flags & MAP_ANONYMOUS) || fd != -1 || length == 0) {
		errno = (length == 0) ? EINVAL : ENODEV;
		return MAP_FAILED;
	}
	void *p = memalign(RB3DS_PAGE_SIZE, length);
	if (p == NULL) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	memset(p, 0, length);
	return p;
}

int munmap(void *addr, size_t length) {
	(void)length;
	free(addr);
	return 0;
}

// No MMU protection available to homebrew: guard pages are not enforced.
int mprotect(void *addr, size_t length, int prot) {
	(void)addr;
	(void)length;
	(void)prot;
	return 0;
}

int madvise(void *addr, size_t length, int advice) {
	(void)addr;
	(void)length;
	(void)advice;
	return 0;
}

// No child processes exist on the 3DS.
pid_t waitpid(pid_t pid, int *status, int options) {
	(void)pid;
	(void)status;
	(void)options;
	errno = ECHILD;
	return -1;
}

int setmode(int fd, int mode) {
	(void)fd;
	(void)mode;
	return 0;
}

// --- Process/user identity: one user, no parent, no child processes. ---------------------
#include <3ds/os.h>
#include <3ds/svc.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }
pid_t getppid(void) { return 0; }

mode_t umask(mode_t mask) {
	(void)mask;
	return 022;
}

struct passwd *getpwnam(const char *name) {
	(void)name;
	return NULL;
}

void endpwent(void) {}

int chown(const char *path, uid_t owner, gid_t group) {
	(void)path;
	(void)owner;
	(void)group;
	errno = ENOSYS;
	return -1;
}

// CPU time: the app core runs only this process, so ARM11 ticks elapsed since the app started
// are a close upper bound; used by Process.times, Process.clock_gettime CPU clocks, GC timing.
static u64 rb3ds_start_tick;

__attribute__((constructor)) static void rb3ds_record_start_tick(void) {
	rb3ds_start_tick = svcGetSystemTick();
}

int getrusage(int who, struct rusage *usage) {
	if (usage == NULL) {
		errno = EFAULT;
		return -1;
	}
	memset(usage, 0, sizeof *usage);
	if (who != RUSAGE_SELF) return 0;
	u64 ticks = svcGetSystemTick() - rb3ds_start_tick;
	// exact conversion without overflowing 64 bits
	u64 us = (ticks / SYSCLOCK_ARM11) * 1000000 + (ticks % SYSCLOCK_ARM11) * 1000000 / SYSCLOCK_ARM11;
	usage->ru_utime.tv_sec = (time_t)(us / 1000000);
	usage->ru_utime.tv_usec = (suseconds_t)(us % 1000000);
	return 0;
}

// --- No signals: Ruby only uses pthread_kill(SIGVTALRM) to interrupt blocking calls. ------
int pthread_kill(pthread_t thread, int sig) {
	(void)thread;
	(void)sig;
	return 0;
}

// --- No pipes or subprocesses. --------------------------------------------------------------
int pipe(int fds[2]) {
	(void)fds;
	errno = ENOSYS;
	return -1;
}

FILE *popen(const char *command, const char *mode) {
	(void)command;
	(void)mode;
	errno = ENOSYS;
	return NULL;
}

int pclose(FILE *stream) {
	(void)stream;
	errno = ECHILD;
	return -1;
}

int execv(const char *path, char *const argv[]) {
	(void)path;
	(void)argv;
	errno = ENOSYS;
	return -1;
}

int execl(const char *path, const char *arg, ...) {
	(void)path;
	(void)arg;
	errno = ENOSYS;
	return -1;
}

int execle(const char *path, const char *arg, ...) {
	(void)path;
	(void)arg;
	errno = ENOSYS;
	return -1;
}

// --- Thread priority (see rb3ds_compat.h). ---------------------------------------------------
#include <3ds/services/apt.h>
#include <3ds/thread.h>

void rb3ds_thread_created(pthread_t thread) {
	s32 prio = 0x30;
	if (R_FAILED(svcGetThreadPriority(&prio, CUR_THREAD_HANDLE))) prio = 0x30;
	// libsysbase's pthread_t is libctru's Thread (libctru source/system/syscalls.c).
	svcSetThreadPriority(threadGetHandle((Thread)thread), prio);
	// Ruby creates its threads with PTHREAD_CREATE_DETACHED and never joins them, but libsysbase
	// ignores that attribute: without this, every finished thread keeps its ~1 MiB stack.
	threadDetach((Thread)thread);
}

void rb3ds_yield(void) {
	svcSleepThread(0);
}
