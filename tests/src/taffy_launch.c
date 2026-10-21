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
// - to the stand-in with "decline": the stand-in declines the image (TAFFY_LAUNCH_DECLINED), and mldr loads and runs
//   it itself. The child prints its argv and environment and exits 42.
// TAFFY_LAUNCH is a Linux path, which mldr dlopen()s. Real use needs an absolute one (taffy_launch.h); this test can't
// name its own directory as a Linux path, so it runs its children in the stand-in's directory, which is also their
// Linux working directory, and gives a path relative to it.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <fcntl.h>
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
// status in *status when it succeeded (-1 when the child didn't exit normally). With out, the child's stdout goes
// there, NUL-terminated.
static int spawn(const char* self, const char* library, const char* mode, int* status, char* out, size_t out_size)
{
	char var[1100];
	char* envp[] = { var, NULL };
	char* argv[] = { (char*) self, (char*) mode, NULL };
	pid_t pid = -1;
	int fds[2] = { -1, -1 };
	posix_spawn_file_actions_t actions;

	snprintf(var, sizeof(var), "TAFFY_LAUNCH=%s", library);
	posix_spawn_file_actions_init(&actions);
	if (out != NULL)
	{
		out[0] = '\0';
		if (pipe(fds) != 0)
			return errno;
		fcntl(fds[0], F_SETFD, FD_CLOEXEC);
		posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
		posix_spawn_file_actions_addclose(&actions, fds[1]);
	}
	int err = posix_spawn(&pid, self, &actions, NULL, argv, envp);
	posix_spawn_file_actions_destroy(&actions);
	*status = -1;
	if (out != NULL)
	{
		close(fds[1]);
		size_t got = 0;
		ssize_t n;
		while (got + 1 < out_size && (n = read(fds[0], out + got, out_size - 1 - got)) > 0)
			got += n;
		out[got] = '\0';
		close(fds[0]);
	}
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
	// a child the launcher declined, which mldr loaded itself: what it was started with, and a status of its own
	if (argc == 2 && strcmp(argv[1], "decline") == 0)
	{
		int mldr_vars = 0;
		for (char** e = environ; *e != NULL; e++)
			mldr_vars += strncmp(*e, "__mldr_", 7) == 0;
		const char* launch = getenv("TAFFY_LAUNCH");
		printf("declined %s %s TAFFY_LAUNCH=%s __mldr_=%d\n", argv[0], argv[1], launch ? launch : "(unset)", mldr_vars);
		return 42;
	}
	// any other child that got here was loaded by mldr itself, which the hook must never do
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
	int err = spawn(self, missing, "run", &status, NULL, 0);
	check(err == EBADEXEC, "a launcher library that doesn't exist fails the spawn with EBADEXEC", err);

	err = spawn(self, stub, "fail", &status, NULL, 0);
	check(err == ENOEXEC, "the launcher's error is posix_spawn()'s", err);

	err = spawn(self, stub, "run", &status, NULL, 0);
	check(err == 0 && status == 0, "the launcher runs with what mldr handed over, and activates", err ? err : status);

	char out[2048], want[2200];
	err = spawn(self, stub, "decline", &status, out, sizeof(out));
	snprintf(want, sizeof(want), "declined %s decline TAFFY_LAUNCH=%s __mldr_=0\n", self, stub);
	check(err == 0 && status == 42, "an image the launcher declines is loaded and run by mldr", err ? err : status);
	check(strcmp(out, want) == 0, "it runs with its own argv, and an environment that keeps TAFFY_LAUNCH", 0);
	if (strcmp(out, want) != 0)
		printf("#   got: %s#   want: %s", out, want);

	return failed;
}
