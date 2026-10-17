// ffsctl(FSIOC_FD_ONLY_OPEN_ONCE) as on XNU: 0 if the descriptor's open is the file's only one, else EBUSY. Darling had
// no ffsctl() (ENOSYS), so MobileInstallation, which asks before it moves a staged file into place, copied every file
// of every install instead ("has open file descriptors; cloning").
//
// Then SIGIO and SIGINFO, which XNU ignores by default and Linux doesn't: Darling ended the process.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks. An optional argument is the
// directory to make the test files in (default: /private/tmp).

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef FSIOC_FD_ONLY_OPEN_ONCE
#define FSIOC_FD_ONLY_OPEN_ONCE _IOWR('A', 21, uint32_t)
#endif

extern int ffsctl(int fd, unsigned long request, void* data, unsigned int options);

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

// 0, or the errno
static int open_once(int fd)
{
	uint32_t arg = 0;

	errno = 0;
	return ffsctl(fd, FSIOC_FD_ONLY_OPEN_ONCE, &arg, 0) == 0 ? 0 : errno;
}

int main(int argc, char** argv)
{
	const char* dir = argc > 1 ? argv[1] : "/private/tmp";
	char path[1024];
	int up[2], down[2];
	char c;

	setvbuf(stdout, NULL, _IOLBF, 0);
	snprintf(path, sizeof(path), "%s/ffsctl_open_once.%d", dir, getpid());

	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	write(fd, "staged", 6);
	check(open_once(fd) == 0, "a file opened once is its only open");

	int d = dup(fd);
	check(open_once(fd) == 0 && open_once(d) == 0, "and a dup() of the descriptor is the same open");
	close(d);

	int second = open(path, O_RDONLY);
	check(open_once(fd) == EBUSY, "another open of the file: EBUSY");
	check(open_once(second) == EBUSY, "and the same from the other one");
	close(second);
	check(open_once(fd) == 0, "the other open closed: 0 again");

	// a mapping keeps its open, as it keeps XNU's vnode in use. Not on overlayfs (a known limit): it maps the file
	// beneath, so the overlay's open goes with its descriptor.
	struct statfs sfs;
	if (fstatfs(fd, &sfs) == 0 && strcmp(sfs.f_fstypename, "overlay") == 0) {
		printf("ok - # skip a mapping of another open (overlayfs: %s)\n", dir);
	} else {
		second = open(path, O_RDONLY);
		void* map = mmap(NULL, 6, PROT_READ, MAP_SHARED, second, 0);
		close(second);
		check(map != MAP_FAILED && open_once(fd) == EBUSY,
		    "a mapping of another open, its descriptor closed: EBUSY");
		munmap(map, 6);
		check(open_once(fd) == 0, "unmapped: 0");
	}

	// another process's open
	pipe(up);
	pipe(down);
	pid_t pid = fork();
	if (pid == 0) {
		int o = open(path, O_RDONLY);
		write(up[1], "x", 1);
		read(down[0], &c, 1);
		close(o);
		_exit(0);
	}
	read(up[0], &c, 1);
	check(open_once(fd) == EBUSY, "another process's open: EBUSY");
	write(down[1], "x", 1);
	waitpid(pid, NULL, 0);
	for (int i = 0; i < 2; i++) {
		close(up[i]);
		close(down[i]);
	}
	check(open_once(fd) == 0, "that process gone: 0");

	errno = 0;
	check(ffsctl(fd, FSIOC_FD_ONLY_OPEN_ONCE, NULL, 0) == -1 && errno == EFAULT, "without an argument: EFAULT");
	errno = 0;
	check(ffsctl(1000, FSIOC_FD_ONLY_OPEN_ONCE, &(uint32_t) { 0 }, 0) == -1 && errno == EBADF,
	    "a descriptor that isn't open: EBADF");
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	errno = 0;
	check(ffsctl(s, FSIOC_FD_ONLY_OPEN_ONCE, &(uint32_t) { 0 }, 0) == -1 && errno == EINVAL, "a socket: EINVAL");
	close(s);

	close(fd);
	unlink(path);

	// XNU ignores SIGIO and SIGINFO by default
	signal(SIGIO, SIG_DFL);
	signal(SIGINFO, SIG_DFL);
	pid = fork();
	if (pid == 0) {
		raise(SIGIO);
		raise(SIGINFO);
		_exit(7);
	}
	int status;
	waitpid(pid, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 7, "SIGIO and SIGINFO are ignored by default");

	return failures;
}
