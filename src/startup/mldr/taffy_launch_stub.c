// A stand-in launcher for tests/src/taffy_launch.c (mldr's TAFFY_LAUNCH hook, taffy_launch.h). mldr loads it into the
// child. What it does depends on the child's argv[1]:
// - "fail": returns ENOEXEC (XNU's 8) without activating. The parent's posix_spawn() should get that error.
// - "decline": reads the executable's header, as a launcher deciding whether to run it would, and declines it
//   (TAFFY_LAUNCH_DECLINED). mldr should then load and run the program itself.
// - "activate-decline": calls activate() with version 0, which mldr refuses, then declines. mldr should fail the
//   spawn: a launcher that has called activate() may no longer decline.
// - "run": checks what mldr handed over, activates, and exits with the number of failed checks. Each failed check
//   prints a "not ok" line, which the test's run-all.sh picks up.
// Like a real launcher, it refuses a handoff of another version or size (taffy_launch.h's lockstep rule).

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "taffy_launch.h"

#define XNU_ENOEXEC 8
#define COMMPAGE64 0x7fffffe00000UL

static int failed;

static void check(int ok, const char* what)
{
	if (!ok)
	{
		fprintf(stderr, "not ok - taffy_launch_stub: %s\n", what);
		failed++;
	}
}

// whether fd is open, close-on-exec, and of type `type` (S_IFDIR, S_IFSOCK, ...)
static int fd_is(int fd, mode_t type)
{
	struct stat st;
	return fd >= 0 && fstat(fd, &st) == 0 && (st.st_mode & S_IFMT) == type && (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
}

// whether all of [start, end) is mapped (/proc/self/maps)
static int is_mapped(uint64_t start, uint64_t end)
{
	FILE* f = fopen("/proc/self/maps", "r");
	if (f == NULL)
		return 0;
	unsigned long lo, hi;
	uint64_t at = start;
	char line[512];
	while (at < end && fgets(line, sizeof(line), f) != NULL)
	{
		if (sscanf(line, "%lx-%lx", &lo, &hi) != 2)
			continue;
		if (lo <= at && at < hi)
			at = hi;
	}
	fclose(f);
	return at >= end;
}

__attribute__((visibility("default")))
int taffy_launch_darling(const struct taffy_launch_darling* l)
{
	if (l->version != TAFFY_LAUNCH_DARLING_VERSION || l->size != sizeof(*l))
		return XNU_ENOEXEC;
	if (l->argc >= 2 && strcmp(l->argv[1], "fail") == 0)
		return XNU_ENOEXEC;
	if (l->argc >= 2 && strcmp(l->argv[1], "decline") == 0)
	{
		uint32_t magic = 0;
		int fd = open(l->executable_path, O_RDONLY | O_CLOEXEC);
		ssize_t n = fd < 0 ? -1 : read(fd, &magic, sizeof(magic));
		if (fd >= 0)
			close(fd);
		return n == sizeof(magic) ? TAFFY_LAUNCH_DECLINED : XNU_ENOEXEC;
	}
	if (l->argc >= 2 && strcmp(l->argv[1], "activate-decline") == 0)
	{
		struct taffy_launch_activation a;
		memset(&a, 0, sizeof(a));
		(void) l->activate(&a);
		return TAFFY_LAUNCH_DECLINED;
	}

	check(l->argc == 2 && l->argv[2] == NULL && strcmp(l->argv[1], "run") == 0,
		"argv is the program's own, without mldr's argv[0]");

	uint64_t envc = 0;
	int has_launch = 0, has_mldr = 0;
	for (; l->envp[envc] != NULL; envc++)
	{
		has_launch |= strncmp(l->envp[envc], TAFFY_LAUNCH_ENV "=", sizeof(TAFFY_LAUNCH_ENV)) == 0;
		has_mldr |= strncmp(l->envp[envc], "__mldr_", 7) == 0;
	}
	check(envc == l->envc, "envc counts envp");
	check(has_launch, "envp keeps " TAFFY_LAUNCH_ENV ", so the program's children use the launcher too");
	check(!has_mldr, "envp has none of mldr's __mldr_ variables");

	check(l->executable_path != NULL && access(l->executable_path, R_OK) == 0,
		"executable_path is a Linux path to the file to execute");

	// the vchroot: the path and the descriptor name the same directory, and the Darwin path is the Linux one below it
	struct stat by_path, by_fd;
	check(l->root_path != NULL && strlen(l->root_path) == l->root_path_length, "root_path and its length");
	check(fd_is(l->root_fd, S_IFDIR) && l->root_path != NULL && stat(l->root_path, &by_path) == 0
		&& fstat(l->root_fd, &by_fd) == 0 && by_path.st_dev == by_fd.st_dev && by_path.st_ino == by_fd.st_ino,
		"root_fd is the vchroot's directory, close-on-exec");
	int next = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	check(next >= 0 && l->root_fd > next, "root_fd isn't where the program's next descriptor goes");
	if (next >= 0)
		close(next);
	check(l->darwin_executable_path != NULL && l->root_path != NULL
		&& strncmp(l->executable_path, l->root_path, l->root_path_length) == 0
		&& strcmp(l->darwin_executable_path, l->executable_path + l->root_path_length) == 0,
		"darwin_executable_path is executable_path below the vchroot");

	check(fd_is(l->kernfd, S_IFSOCK), "kernfd is mldr's darlingserver socket, close-on-exec");
	char want_kernfd[32], want_elf_calls[48];
	snprintf(want_kernfd, sizeof(want_kernfd), "kernfd=%d", l->kernfd);
	snprintf(want_elf_calls, sizeof(want_elf_calls), "elf_calls=%lx", (unsigned long) l->elf_calls);
	check(l->elf_calls != NULL && l->apple_extra != NULL && l->apple_extra[0] != NULL && l->apple_extra[1] != NULL
		&& l->apple_extra[2] == NULL && strcmp(l->apple_extra[0], want_kernfd) == 0
		&& strcmp(l->apple_extra[1], want_elf_calls) == 0,
		"apple_extra is kernfd= and elf_calls=, as mldr's own apple[] has them");

	// the ranges: in address order, each really mapped, and the commpage and mldr's stack among them
	int sorted = l->mapped != NULL && l->mapped_count >= 2, all_mapped = sorted, commpage = 0, stack = 0;
	for (uint32_t i = 0; sorted && i < l->mapped_count; i++)
	{
		const struct taffy_launch_range* r = &l->mapped[i];
		sorted &= r->start < r->end && (i == 0 || l->mapped[i - 1].end <= r->start) && r->reserved == 0;
		all_mapped &= is_mapped(r->start, r->end);
		if (r->kind == TAFFY_LAUNCH_RANGE_COMMPAGE)
			commpage = r->start == COMMPAGE64 && memcmp((const void*)(uintptr_t) r->start, "commpage 64-bit", 15) == 0;
		if (r->kind == TAFFY_LAUNCH_RANGE_MLDR_STACK)
			stack = r->end == COMMPAGE64;
	}
	check(sorted, "mapped is in address order, without overlaps");
	check(all_mapped, "every range in mapped is mapped (/proc/self/maps)");
	check(commpage, "mapped has the commpage, filled in");
	check(stack, "mapped has the stack mldr maps for the program, ending at the commpage");

	struct taffy_launch_activation a;
	memset(&a, 0, sizeof(a));
	a.version = TAFFY_LAUNCH_ACTIVATION_VERSION;
	a.size = sizeof(a);
	a.main_stack_top = COMMPAGE64;
	check(l->activate(&a) == 0, "activate() tells darlingserver and returns 0");
	check(l->activate(&a) != 0, "a second activate() is refused");

	fflush(stderr);
	_exit(failed);
}
