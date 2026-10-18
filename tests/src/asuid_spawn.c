// simasuid (src/ios-sim) runs a program as another user, and simboot.sh relies on everything that program starts
// staying that user: a device's launchd_sim, and every process it spawns. Darling keeps the ids in darlingserver,
// which copies them to a fork and keeps them across exec. Checks a process simasuid starts, and one that process
// posix_spawn()s, have the uid, the gid, and only the gid as their group; and simasuid's exit codes.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#define SIMASUID "/usr/libexec/ios-sim/simasuid"

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	fflush(stdout);
	if (!ok)
		failures++;
}

// writes "uid euid gid egid ngroups group0" to fd. The groups are the process's own, as setgroups() set them: the
// system call, not libc's getgroups(), which answers the user's groups from the directory (getgroups(2)).
static void report(int fd)
{
	gid_t groups[NGROUPS_MAX];
	int n = syscall(SYS_getgroups, NGROUPS_MAX, groups);
	dprintf(fd, "%d %d %d %d %d %d\n", (int) getuid(), (int) geteuid(), (int) getgid(), (int) getegid(), n,
		n > 0 ? (int) groups[0] : -1);
}

static int spawn_wait(char* const argv[])
{
	pid_t pid;
	int status;
	if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0)
		return -1;
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

int main(int argc, char** argv)
{
	char self[MAXPATHLEN];
	uint32_t n = sizeof(self);
	_NSGetExecutablePath(self, &n);

	// as simasuid's program: report, and have a child report too
	if (argc == 3 && strcmp(argv[1], "child") == 0)
	{
		int fd = atoi(argv[2]);
		report(fd);
		char* cargv[] = { self, "report", argv[2], NULL };
		return spawn_wait(cargv) == 0 ? 0 : 1;
	}
	if (argc == 3 && strcmp(argv[1], "report") == 0)
	{
		report(atoi(argv[2]));
		return 0;
	}

	if (access(SIMASUID, X_OK) != 0)
	{
		printf("ok - # SKIP no %s (the ios_sim component)\n", SIMASUID);
		return 0;
	}

	int p[2];
	char fd[16];
	pipe(p);
	snprintf(fd, sizeof(fd), "%d", p[1]);
	char* aargv[] = { SIMASUID, "501", "20", self, "child", fd, NULL };
	int rc = spawn_wait(aargv);
	close(p[1]);
	char buf[256] = "";
	FILE* f = fdopen(p[0], "r");
	int got[2][6];
	int lines = 0;
	while (lines < 2 && fgets(buf, sizeof(buf), f))
	{
		if (sscanf(buf, "%d %d %d %d %d %d", &got[lines][0], &got[lines][1], &got[lines][2], &got[lines][3],
				&got[lines][4], &got[lines][5]) == 6)
			lines++;
	}
	fclose(f);
	check(rc == 0 && lines == 2, "simasuid 501 20 runs the program, which spawns a child");
	for (int i = 0; i < lines; i++)
	{
		const char* who = i == 0 ? "the program" : "its child";
		char what[128];
		snprintf(what, sizeof(what), "%s: uid and euid 501", who);
		check(got[i][0] == 501 && got[i][1] == 501, what);
		snprintf(what, sizeof(what), "%s: gid and egid 20", who);
		check(got[i][2] == 20 && got[i][3] == 20, what);
		// a regression guard: root's group 0 stayed among the groups
		snprintf(what, sizeof(what), "%s: its only group is 20", who);
		check(got[i][4] == 1 && got[i][5] == 20, what);
		if (got[i][4] != 1 || got[i][5] != 20)
			printf("# %d groups, the first %d\n", got[i][4], got[i][5]);
	}

	char* bad_uid[] = { SIMASUID, "mobile", "20", "/usr/bin/true", NULL };
	check(spawn_wait(bad_uid) == 64, "a uid that isn't a number: exit 64");
	char* big_uid[] = { SIMASUID, "2147483648", "20", "/usr/bin/true", NULL };
	check(spawn_wait(big_uid) == 64, "a uid over 2147483647: exit 64");
	char* no_prog[] = { SIMASUID, "501", "20", "/no/such/program", NULL };
	check(spawn_wait(no_prog) == 127, "a program that doesn't exist: exit 127");
	return failures;
}
