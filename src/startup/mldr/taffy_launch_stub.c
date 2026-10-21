// A stand-in launcher for tests/src/taffy_launch.c (mldr's TAFFY_LAUNCH hook, taffy_launch.h). mldr loads it into the
// child. What it does depends on the child's argv[1]:
// - "fail": returns ENOEXEC (XNU's 8) without activating. The parent's posix_spawn() should get that error.
// - "run": checks what mldr handed over, activates, and exits with the number of failed checks. Each failed check
//   prints a "not ok" line, which the test's run-all.sh picks up.

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

// whether fd is open and of type `type` (S_IFDIR, S_IFSOCK, ...)
static int fd_is(int fd, mode_t type)
{
	struct stat st;
	return fd >= 0 && fstat(fd, &st) == 0 && (st.st_mode & S_IFMT) == type;
}

__attribute__((visibility("default")))
int taffy_launch_darling(const struct taffy_launch_darling* l)
{
	if (l->argc >= 2 && strcmp(l->argv[1], "fail") == 0)
		return XNU_ENOEXEC;

	check(l->version == TAFFY_LAUNCH_DARLING_VERSION && l->size == sizeof(*l), "the handoff's version and size");
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

	// the vchroot: the path and the descriptor name the same directory
	struct stat by_path, by_fd;
	check(l->root_path != NULL && strlen(l->root_path) == l->root_path_length, "root_path and its length");
	check(fd_is(l->root_fd, S_IFDIR) && l->root_path != NULL && stat(l->root_path, &by_path) == 0
		&& fstat(l->root_fd, &by_fd) == 0 && by_path.st_dev == by_fd.st_dev && by_path.st_ino == by_fd.st_ino,
		"root_fd is the vchroot's directory");
	check((fcntl(l->root_fd, F_GETFD) & FD_CLOEXEC) != 0, "root_fd is close-on-exec");
	check(l->system_root != NULL && l->system_root[0] == '/', "system_root");

	check(fd_is(l->kernfd, S_IFSOCK), "kernfd is mldr's darlingserver socket");
	check(l->elf_calls != NULL, "elf_calls");

	check(l->commpage_address == COMMPAGE64 && memcmp((const void*) l->commpage_address, "commpage 64-bit", 15) == 0,
		"the commpage is mapped and filled in");
	check(l->mldr_stack_top == l->commpage_address, "mldr's stack ends at the commpage");

	struct taffy_launch_activation a;
	memset(&a, 0, sizeof(a));
	a.version = TAFFY_LAUNCH_ACTIVATION_VERSION;
	a.size = sizeof(a);
	a.main_stack_top = l->mldr_stack_top;
	check(l->activate(&a) == 0, "activate() tells darlingserver and returns 0");

	fflush(stderr);
	_exit(failed);
}
