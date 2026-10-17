// gethostuuid() as on XNU: the host's UUID, the same for every process (an iOS Simulator's processes get their Mac's).
// Darling read it from /etc/machine-id and failed with ENOSYS when that was empty, as a container's often is
// (Debian's images ship it empty): installd, lsd and SpringBoard got ENOSYS, and SQLite's proxy locking names its
// lock's host with it.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

extern char** environ;

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

// another process's answer: this program run again, printing the UUID it gets
static int other_process_uuid(const char* self, uuid_t uuid)
{
	int p[2], status;
	pid_t pid;
	char text[37] = { 0 };
	char* argv[] = { (char*) self, "print", NULL };
	posix_spawn_file_actions_t fa;

	if (pipe(p) != 0)
		return -1;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	posix_spawn_file_actions_addclose(&fa, p[0]);
	if (posix_spawn(&pid, self, &fa, NULL, argv, environ) != 0)
		return -1;
	posix_spawn_file_actions_destroy(&fa);
	close(p[1]);
	ssize_t n = read(p[0], text, 36);
	close(p[0]);
	waitpid(pid, &status, 0);
	if (n != 36 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return uuid_parse(text, uuid);
}

int main(int argc, char** argv)
{
	struct timespec wait = { 1, 0 };
	uuid_t uuid, again, other, zero = { 0 };
	char text[37];

	if (argc > 1 && strcmp(argv[1], "print") == 0) {
		if (gethostuuid(uuid, &wait) != 0)
			return 1;
		uuid_unparse(uuid, text);
		fputs(text, stdout);
		return 0;
	}

	setvbuf(stdout, NULL, _IOLBF, 0);

	errno = 0;
	int ret = gethostuuid(uuid, &wait);
	check(ret == 0, "gethostuuid() succeeds");
	if (ret != 0)
		printf("# errno %d (%s)\n", errno, strerror(errno));
	check(ret == 0 && uuid_compare(uuid, zero) != 0, "and the UUID isn't all zeros");

	check(gethostuuid(again, &wait) == 0 && uuid_compare(uuid, again) == 0, "a second call gets the same UUID");
	check(other_process_uuid(argv[0], other) == 0 && uuid_compare(uuid, other) == 0,
	    "another process gets the same UUID");

	// XNU copies the timeout in: without one, EFAULT
	errno = 0;
	check(gethostuuid(again, NULL) == -1 && errno == EFAULT, "gethostuuid() without a timeout fails with EFAULT");

	return failures;
}
