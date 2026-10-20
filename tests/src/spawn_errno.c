// posix_spawn() fails with XNU's errno, whichever way it runs (B61). Darling's posix_spawn converted the error of a
// POSIX_SPAWN_SETEXEC spawn from Linux's numbers to XNU's when it was XNU's already, so wherever the two differ it
// returned another error: ELOOP (62) came back as ETIME, ENAMETOOLONG (63) as ENOSR.
//
// - A path that is a symlink loop: ELOOP from a spawn that forks, and from a POSIX_SPAWN_SETEXEC one, which fails
//   and returns here.
// It uses only XNU's interface.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char** environ;

static int failed;

static void check(int ok, const char* what, int err)
{
	printf("%s - %s (%d, %s)\n", ok ? "ok" : "not ok", what, err, strerror(err));
	if (!ok)
		failed++;
}

static int spawn(const char* path, short flags)
{
	posix_spawnattr_t attr;
	char* argv[] = { (char*) path, NULL };
	pid_t pid = -1;
	int err;

	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, flags);
	err = posix_spawn(&pid, path, NULL, &attr, argv, environ);
	posix_spawnattr_destroy(&attr);
	return err;
}

int main(void)
{
	char dir[] = "/private/tmp/spawn_errno.XXXXXX";
	char a[64], b[64];

	if (mkdtemp(dir) == NULL)
	{
		perror("mkdtemp");
		return 1;
	}
	snprintf(a, sizeof(a), "%s/a", dir);
	snprintf(b, sizeof(b), "%s/b", dir);
	if (symlink(b, a) != 0 || symlink(a, b) != 0)
	{
		perror("symlink");
		return 1;
	}

	int err = spawn(a, 0);
	check(err == ELOOP, "a spawn of a symlink loop fails with ELOOP", err);
	err = spawn(a, POSIX_SPAWN_SETEXEC);
	check(err == ELOOP, "a POSIX_SPAWN_SETEXEC spawn of a symlink loop fails with ELOOP", err);

	unlink(a);
	unlink(b);
	rmdir(dir);
	return failed;
}
