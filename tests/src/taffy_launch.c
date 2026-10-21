// mldr's TAFFY_LAUNCH hook (src/startup/mldr/taffy_launch.h): with the variable set, mldr hands the process to the
// library it names instead of loading the image. A launcher that can't be loaded must fail the spawn, never fall back
// to mldr's own loading.
//
// It spawns itself with TAFFY_LAUNCH set:
// - to a library that doesn't exist: posix_spawn() fails with EBADEXEC, and nothing runs.
// - to the stand-in launcher (src/startup/mldr/taffy_launch_stub.c) with argv[1] "fail": posix_spawn() returns
//   the launcher's error, ENOEXEC.
// - to the stand-in with "run": posix_spawn() succeeds, and the child exits 0. The stand-in checks what mldr handed
//   over (argv, envp, the vchroot, kernfd, elf_calls, the commpage), activates, and exits with its number of failures.
// TAFFY_LAUNCH is a Linux path, which mldr dlopen()s. Real use needs an absolute one (taffy_launch.h); this test can't
// name its own directory as a Linux path, so it runs its children in the stand-in's directory, which is also their
// Linux working directory, and gives a path relative to it.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <libgen.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
extern int _NSGetExecutablePath(char* buf, uint32_t* size); // <mach-o/dyld.h>

static int failed;

static void check(int ok, const char* what, int err)
{
	printf("%s - %s (%d)\n", ok ? "ok" : "not ok", what, err);
	if (!ok)
		failed++;
}

// spawns this program with TAFFY_LAUNCH=library and argv[1] = mode. Returns posix_spawn()'s error, and the exit
// status in *status when it succeeded (-1 when the child didn't exit normally).
static int spawn(const char* self, const char* library, const char* mode, int* status)
{
	char var[1100];
	char* envp[] = { var, NULL };
	char* argv[] = { (char*) self, (char*) mode, NULL };
	pid_t pid = -1;

	snprintf(var, sizeof(var), "TAFFY_LAUNCH=%s", library);
	int err = posix_spawn(&pid, self, NULL, NULL, argv, envp);
	*status = -1;
	if (err == 0)
	{
		int st = 0;
		if (waitpid(pid, &st, 0) == pid && WIFEXITED(st))
			*status = WEXITSTATUS(st);
	}
	return err;
}

int main(int argc, char** argv)
{
	(void) argv;
	// a child that got here was loaded by mldr itself, which the hook must never do
	if (argc > 1)
		return 99;

	char self[1024];
	uint32_t size = sizeof(self);
	if (_NSGetExecutablePath(self, &size) != 0)
	{
		check(0, "setup: _NSGetExecutablePath", 0);
		return failed;
	}
	char dir[1024];
	strncpy(dir, self, sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = '\0';
	if (chdir(dirname(dir)) != 0)
	{
		check(0, "setup: chdir to the test's directory", errno);
		return failed;
	}

	int status;
	const char* missing = "./taffy_launch_missing.so";
	const char* stub = "./taffy_launch_stub.so";
	int err = spawn(self, missing, "run", &status);
	check(err == EBADEXEC, "a launcher library that doesn't exist fails the spawn with EBADEXEC", err);

	err = spawn(self, stub, "fail", &status);
	check(err == ENOEXEC, "the launcher's error is posix_spawn()'s", err);

	err = spawn(self, stub, "run", &status);
	check(err == 0 && status == 0, "the launcher runs with what mldr handed over, and activates", err ? err : status);

	return failed;
}
