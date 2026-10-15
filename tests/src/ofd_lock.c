// Locks of an open file description (F_OFD_SETLK, F_OFD_GETLK), as XNU has them. Apple's SQLite probes a file
// system's locking with F_OFD_GETLK and fell back to dot-file locks when Darling refused it; a device's databases then
// stayed locked after a daemon died holding them, and the device couldn't boot again.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef F_OFD_SETLK
#	define F_OFD_SETLK 90
#	define F_OFD_GETLK 92
#	define F_OFD_SETLKWTIMEOUT 93
#endif

struct flocktimeout_
{
	struct flock fl;
	struct timespec timeout;
};

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

int main(void)
{
	const char* base = getenv("TMPDIR") ? getenv("TMPDIR") : ".";
	char file[PATH_MAX];

	snprintf(file, sizeof(file), "%s/ofd_lock.XXXXXX", base);
	int a = mkstemp(file);
	if (a < 0)
	{
		perror("mkstemp");
		return 1;
	}
	// another open file description of the same file, in the same process
	int b = open(file, O_RDWR);

	struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1 };
	check(fcntl(a, F_OFD_SETLK, &lock) == 0, "F_OFD_SETLK takes a lock");

	struct flock probe = { .l_type = F_RDLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1 };
	check(fcntl(b, F_OFD_GETLK, &probe) == 0 && probe.l_type == F_WRLCK,
		"F_OFD_GETLK sees it from another open file description");

	struct flock other = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1 };
	errno = 0;
	check(fcntl(b, F_OFD_SETLK, &other) == -1 && (errno == EAGAIN || errno == EACCES),
		"which can't take it, even in the same process");

	// waiting for it gives up after the timeout: SQLite waits for its locks this way
	struct flocktimeout_ wait = { other, { 0, 100000000 } };
	errno = 0;
	int rv = fcntl(b, F_OFD_SETLKWTIMEOUT, &wait);
	check(rv == -1 && errno == ETIMEDOUT, "F_OFD_SETLKWTIMEOUT gives up with ETIMEDOUT");

	close(a);
	check(fcntl(b, F_OFD_SETLKWTIMEOUT, &wait) == 0, "and takes the lock once it's free");

	close(b);
	unlink(file);
	return failures;
}
