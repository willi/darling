// A process's names, as XNU's exec records them (set_proc_name(), xnu-12377.121.6 bsd/kern/kern_exec.c): the last
// component of the executable it ran, in p_name (up to 2 * MAXCOMLEN characters) and p_comm (up to MAXCOMLEN), which a
// fork keeps until it execs, and which a #! script gets from its interpreter. proc_pidinfo() copies them into pbi_name,
// pbi_comm and pbsi_comm, each with its last byte made a NUL (bsd/kern/proc_info.c), sysctl's kinfo_proc into p_comm
// (bsd/kern/kern_sysctl.c), and proc_name() answers pbi_name. Darling answered Linux's comm instead, which is "mldr"
// for every process: backboardd logged SpringBoard as "mldr.577".
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <copyfile.h>
#include <errno.h>
#include <libproc.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	fflush(stdout);
	if (!ok)
		failures++;
}

static void check_str(const char* got, const char* want, const char* what)
{
	check(strcmp(got, want) == 0, what);
	if (strcmp(got, want) != 0)
		printf("# got \"%s\", want \"%s\"\n", got, want);
}

// the first n characters of s
static const char* prefix(const char* s, size_t n, char* buf)
{
	snprintf(buf, n + 1, "%s", s);
	return buf;
}

// every name XNU reports for pid, against the executable name it ran
static void check_names(pid_t pid, const char* exe, const char* who)
{
	char what[256], want[64], name[2 * MAXCOMLEN + 1] = "";
	struct proc_bsdinfo bi;
	struct proc_bsdshortinfo si;
	struct kinfo_proc kp;
	size_t kl = sizeof(kp);
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };

	memset(&bi, 0, sizeof(bi));
	memset(&si, 0, sizeof(si));
	memset(&kp, 0, sizeof(kp));
	proc_name(pid, name, sizeof(name));
	proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi));
	proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &si, sizeof(si));
	sysctl(mib, 4, &kp, &kl, NULL, 0);

	snprintf(what, sizeof(what), "%s: proc_name() is the executable's first 2*MAXCOMLEN-1 characters", who);
	check_str(name, prefix(exe, 2 * MAXCOMLEN - 1, want), what);
	snprintf(what, sizeof(what), "%s: pbi_name is the executable's first 2*MAXCOMLEN-1 characters", who);
	check_str(bi.pbi_name, prefix(exe, 2 * MAXCOMLEN - 1, want), what);
	snprintf(what, sizeof(what), "%s: pbi_comm is its first MAXCOMLEN-1", who);
	check_str(bi.pbi_comm, prefix(exe, MAXCOMLEN - 1, want), what);
	snprintf(what, sizeof(what), "%s: pbsi_comm is its first MAXCOMLEN-1", who);
	check_str(si.pbsi_comm, prefix(exe, MAXCOMLEN - 1, want), what);
	snprintf(what, sizeof(what), "%s: kinfo_proc's p_comm is its first MAXCOMLEN", who);
	check_str(kp.kp_proc.p_comm, prefix(exe, MAXCOMLEN, want), what);
}

// waits (up to 10 s) for pid to have exec'd the executable named exe, by its path
static void wait_for_exec(pid_t pid, const char* exe)
{
	char path[PROC_PIDPATHINFO_MAXSIZE];
	for (int i = 0; i < 200; i++)
	{
		if (proc_pidpath(pid, path, sizeof(path)) > 0)
		{
			const char* base = strrchr(path, '/');
			if (strcmp(base ? base + 1 : path, exe) == 0)
				return;
		}
		usleep(50000);
	}
}

// runs path (named exe) with "wait" until its stdin closes, and checks its names from here
static void spawned(const char* path, const char* exe, const char* who)
{
	int in[2];
	pid_t pid;
	posix_spawn_file_actions_t fa;
	char* argv[] = { (char*) path, "wait", (char*) exe, NULL };

	pipe(in);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, in[0], 0);
	posix_spawn_file_actions_addclose(&fa, in[1]);
	if (posix_spawn(&pid, path, &fa, NULL, argv, environ) != 0)
	{
		check(0, who);
		return;
	}
	posix_spawn_file_actions_destroy(&fa);
	close(in[0]);
	wait_for_exec(pid, exe);
	check_names(pid, exe, who);
	close(in[1]);
	int status = 0;
	waitpid(pid, &status, 0);
	// the child checked its own names too, and exits with its failures
	failures += WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int main(int argc, char** argv)
{
	char self[PATH_MAX], dir[PATH_MAX], path[PATH_MAX], script[PATH_MAX], interp[PATH_MAX], what[256];
	uint32_t n = sizeof(self);
	const char* me;

	// a spawned copy: check our own names, then wait for stdin to close
	if (argc == 3 && strcmp(argv[1], "wait") == 0)
	{
		snprintf(what, sizeof(what), "%s, about itself", argv[2]);
		check_names(getpid(), argv[2], what);
		char c;
		while (read(0, &c, 1) > 0)
			;
		return failures;
	}

	_NSGetExecutablePath(self, &n);
	me = strrchr(self, '/') ? strrchr(self, '/') + 1 : self;
	snprintf(dir, sizeof(dir), "%s/proc_name.XXXXXX", "/private/tmp");
	if (!mkdtemp(dir))
	{
		perror("mkdtemp");
		return 1;
	}

	check_names(getpid(), me, "this process");

	// an executable's name, 2 characters, over MAXCOMLEN and over 2 * MAXCOMLEN
	static const char* names[] = { "pn", "proc_name_seventeen", "proc_name_with_a_name_over_thirty_two_characters" };
	for (int i = 0; i < 3; i++)
	{
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		copyfile(self, path, NULL, COPYFILE_DATA);
		chmod(path, 0755);
		snprintf(what, sizeof(what), "a %zu-character name, spawned", strlen(names[i]));
		spawned(path, names[i], what);
	}

	// a fork keeps its parent's names until it execs
	snprintf(path, sizeof(path), "%s/%s", dir, names[0]);
	int in[2], ready[2];
	pipe(in);
	pipe(ready);
	pid_t pid = fork();
	if (pid == 0)
	{
		char c;
		close(in[1]);
		close(ready[0]);
		write(ready[1], "x", 1);
		// wait for the parent's look, then exec the short name (which waits for stdin to close)
		read(in[0], &c, 1);
		dup2(in[0], 0);
		execl(path, path, "wait", names[0], (char*) NULL);
		_exit(127);
	}
	close(in[0]);
	close(ready[1]);
	char c;
	read(ready[0], &c, 1);
	check_names(pid, me, "a fork, before it execs");
	write(in[1], "x", 1);
	wait_for_exec(pid, names[0]);
	check_names(pid, names[0], "the fork, after it execs");
	close(in[1]);
	int status = 0;
	waitpid(pid, &status, 0);
	failures += WIFEXITED(status) ? WEXITSTATUS(status) : 1;

	// a #! script runs as its interpreter, whose (resolved) name it gets
	snprintf(script, sizeof(script), "%s/proc_name_script", dir);
	FILE* f = fopen(script, "w");
	fprintf(f, "#!/bin/sh\nread x\n");
	fclose(f);
	chmod(script, 0755);
	const char* interp_name = realpath("/bin/sh", interp) ? (strrchr(interp, '/') + 1) : "sh";
	{
		int sin[2];
		posix_spawn_file_actions_t fa;
		char* sargv[] = { script, NULL };
		pipe(sin);
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_adddup2(&fa, sin[0], 0);
		posix_spawn_file_actions_addclose(&fa, sin[1]);
		if (posix_spawn(&pid, script, &fa, NULL, sargv, environ) == 0)
		{
			close(sin[0]);
			wait_for_exec(pid, interp_name);
			snprintf(what, sizeof(what), "a #! script, as its interpreter %s", interp_name);
			check_names(pid, interp_name, what);
			close(sin[1]);
			waitpid(pid, NULL, 0);
		}
		else
			check(0, "spawning a #! script");
		posix_spawn_file_actions_destroy(&fa);
	}

	unlink(script);
	for (int i = 0; i < 3; i++)
	{
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		unlink(path);
	}
	rmdir(dir);
	return failures;
}
