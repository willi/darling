// ffsctl(FSIOC_FD_ONLY_OPEN_ONCE) as on XNU: 0 if the descriptor's open is the file's only one, else EBUSY. Darling had
// no ffsctl() (ENOSYS), so MobileInstallation, which asks before it moves a staged file into place, copied every file
// of every install instead ("has open file descriptors; cloning"). Then the other answers XNU's fsctl_internal()
// gives, and fsctl() by path.
//
// Then the signals XNU ignores by default: Darling ended the process on SIGIO, never ran a SIGINFO handler (it didn't
// know SIGINFO's Linux signal), and once SIGWINCH was set back to its default, never ran a handler set later.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks. An optional argument is the
// directory to make the test files in (default: /private/tmp). On overlayfs (the prefix's own), the checks run again in
// the host's /dev/shm, a tmpfs, for the mapping check overlayfs can't pass.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fsctl.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef FSIOC_FD_ONLY_OPEN_ONCE
#define FSIOC_FD_ONLY_OPEN_ONCE _IOWR('A', 21, uint32_t)
#endif

extern int ffsctl(int fd, unsigned long request, void* data, unsigned int options);

static int failures;

static void check(int ok, const char* what, const char* where)
{
	printf("%s - %s%s%s\n", ok ? "ok" : "not ok", what, where ? ": " : "", where ? where : "");
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

static int is_overlay(const char* dir)
{
	struct statfs sfs;

	return statfs(dir, &sfs) == 0 && strcmp(sfs.f_fstypename, "overlay") == 0;
}

static void open_checks(const char* dir)
{
	char path[1024];
	int up[2], down[2];
	char c;

	snprintf(path, sizeof(path), "%s/ffsctl_open_once.%d", dir, getpid());
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	write(fd, "staged", 6);
	check(open_once(fd) == 0, "a file opened once is its only open", dir);
	// the answer leaves nothing behind: a lease kept would make this open wait, or fail as here
	int other = open(path, O_RDONLY | O_NONBLOCK);
	check(other >= 0, "and another open right after it isn't held up", dir);
	close(other);

	int d = dup(fd);
	check(open_once(fd) == 0 && open_once(d) == 0, "a dup() of the descriptor is the same open", dir);
	close(d);

	int second = open(path, O_RDONLY);
	check(open_once(fd) == EBUSY, "another open of the file: EBUSY", dir);
	check(open_once(second) == EBUSY, "and the same from the other one", dir);
	close(second);
	check(open_once(fd) == 0, "the other open closed: 0 again", dir);

	// a mapping takes a use count of the vnode on XNU (ubc_map()), even one made through this very open
	void* map = mmap(NULL, 6, PROT_READ, MAP_SHARED, fd, 0);
	check(map != MAP_FAILED && open_once(fd) == EBUSY, "a mapping through the same descriptor: EBUSY", dir);
	munmap(map, 6);
	check(open_once(fd) == 0, "unmapped: 0", dir);

	// Not on overlayfs (a known limit): it maps the file beneath, so the overlay's open goes with its descriptor.
	if (is_overlay(dir)) {
		printf("ok - # skip a mapping of another open whose descriptor is closed: %s (overlayfs)\n", dir);
	} else {
		second = open(path, O_RDONLY);
		map = mmap(NULL, 6, PROT_READ, MAP_SHARED, second, 0);
		close(second);
		check(map != MAP_FAILED && open_once(fd) == EBUSY, "a mapping of another open, its descriptor closed: EBUSY",
		    dir);
		munmap(map, 6);
		check(open_once(fd) == 0, "unmapped: 0", dir);
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
	check(open_once(fd) == EBUSY, "another process's open: EBUSY", dir);
	write(down[1], "x", 1);
	waitpid(pid, NULL, 0);
	for (int i = 0; i < 2; i++) {
		close(up[i]);
		close(down[i]);
	}
	check(open_once(fd) == 0, "that process gone: 0", dir);
	close(fd);

	// a read-only open asks as well
	fd = open(path, O_RDONLY);
	check(open_once(fd) == 0, "a file opened once, read-only, is its only open", dir);
	close(fd);
	unlink(path);
}

static volatile sig_atomic_t caught;

static void catch(int sig)
{
	caught = sig;
}

// a fork child raises sig at its default disposition, and must live on
static int survives(int sig)
{
	int status;
	pid_t pid = fork();

	if (pid == 0) {
		signal(sig, SIG_DFL);
		raise(sig);
		_exit(7);
	}
	waitpid(pid, &status, 0);
	return WIFEXITED(status) && WEXITSTATUS(status) == 7;
}

int main(int argc, char** argv)
{
	const char* dir = argc > 1 ? argv[1] : "/private/tmp";
	const char* tmpfs = "/Volumes/SystemRoot/dev/shm";
	char path[1024];

	setvbuf(stdout, NULL, _IOLBF, 0);

	open_checks(dir);
	if (is_overlay(dir) && access(tmpfs, W_OK) == 0 && !is_overlay(tmpfs))
		open_checks(tmpfs);

	snprintf(path, sizeof(path), "%s/ffsctl_open_once.%d", dir, getpid());
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	errno = 0;
	check(ffsctl(fd, FSIOC_FD_ONLY_OPEN_ONCE, NULL, 0) == -1 && errno == EFAULT, "without an argument: EFAULT", NULL);
	errno = 0;
	check(ffsctl(fd, FSIOC_FD_ONLY_OPEN_ONCE, (void*) 8, 0) == -1 && errno == EFAULT, "a bad argument: EFAULT", NULL);
	errno = 0;
	check(fsctl(path, FSIOC_FD_ONLY_OPEN_ONCE, &(uint32_t) { 0 }, 0) == -1 && errno == EINVAL,
	    "by path (fsctl()), where no open can be the caller's: EINVAL", NULL);
	uint32_t flags = FSCTL_SYNC_WAIT;
	check(ffsctl(fd, FSIOC_SYNC_VOLUME, &flags, 0) == 0 && fsctl(dir, FSIOC_SYNC_VOLUME, &flags, 0) == 0,
	    "FSIOC_SYNC_VOLUME by descriptor and by path", NULL);
	errno = 0;
	check(ffsctl(fd, _IO('z', 99), NULL, 0) == -1 && errno == ENOTTY, "a command no one knows: ENOTTY", NULL);
	close(fd);
	unlink(path);

	errno = 0;
	check(ffsctl(1000, FSIOC_FD_ONLY_OPEN_ONCE, &(uint32_t) { 0 }, 0) == -1 && errno == EBADF,
	    "a descriptor that isn't open: EBADF", NULL);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	errno = 0;
	check(ffsctl(s, FSIOC_FD_ONLY_OPEN_ONCE, &(uint32_t) { 0 }, 0) == -1 && errno == EINVAL, "a socket: EINVAL", NULL);
	close(s);
	int null = open("/dev/null", O_RDONLY);
	errno = 0;
	check(ffsctl(null, FSIOC_FD_ONLY_OPEN_ONCE, &(uint32_t) { 0 }, 0) == -1 && errno == ENOTTY,
	    "a device: ENOTTY", NULL);
	close(null);

	// XNU ignores SIGIO and SIGINFO by default (sigprop)
	check(survives(SIGIO), "SIGIO is ignored by default", NULL);
	check(survives(SIGINFO), "SIGINFO is ignored by default", NULL);
	signal(SIGINFO, catch);
	caught = 0;
	raise(SIGINFO);
	check(caught == SIGINFO, "a SIGINFO handler runs", NULL);
	// SIGWINCH set back to its default, then a handler: it runs
	signal(SIGWINCH, SIG_DFL);
	raise(SIGWINCH);
	signal(SIGWINCH, catch);
	caught = 0;
	raise(SIGWINCH);
	check(caught == SIGWINCH, "a SIGWINCH handler set after SIG_DFL runs", NULL);

	return failures;
}
