// A process's names, as XNU's exec records them (set_proc_name(), xnu-12377.121.6 bsd/kern/kern_exec.c): the last
// component of the executable it ran, in p_name (up to 2 * MAXCOMLEN characters) and p_comm (up to MAXCOMLEN), which a
// fork keeps until it execs, and which a #! script gets from its interpreter. proc_pidinfo() copies them into pbi_name,
// pbi_comm and pbsi_comm, each with its last byte made a NUL (bsd/kern/proc_info.c), sysctl's kinfo_proc into p_comm
// (bsd/kern/kern_sysctl.c), and proc_name() answers pbi_name. Darling answered Linux's comm instead, which is "mldr"
// for every process: backboardd logged SpringBoard as "mldr.577".
//
// posix_spawn() returns once the new image is activated (so the names are already set), or with the error that failed
// the activation, leaving no process. A zombie is found by PROC_PIDTBSDINFO only when its arg asks for one.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <copyfile.h>
#include <errno.h>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <libproc.h>
#include <mach/machine.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/resource.h>
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

// file actions that close stderr, then dup over the descriptor posix_spawn()'s own pipe gets next
static void over_the_pipe(posix_spawn_file_actions_t* fa, int source)
{
	int lowest = dup(0);
	close(lowest);
	posix_spawn_file_actions_addclose(fa, 2);
	posix_spawn_file_actions_adddup2(fa, source, lowest + 1);
}

enum spawn_how
{
	SPAWN_PLAIN,
	// file actions that close stderr, then dup over the descriptor posix_spawn()'s own pipe gets
	SPAWN_OVER_THE_PIPE,
	// POSIX_SPAWN_CLOEXEC_DEFAULT, which closes what no action inherits
	SPAWN_CLOEXEC_DEFAULT,
};

// runs path (named exe) with "wait" until its stdin closes, and checks its names from here
static void spawned_how(const char* path, const char* exe, const char* who, enum spawn_how how)
{
	int in[2];
	pid_t pid;
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t attr;
	char* argv[] = { (char*) path, "wait", (char*) exe, NULL };

	pipe(in);
	posix_spawn_file_actions_init(&fa);
	posix_spawnattr_init(&attr);
	posix_spawn_file_actions_adddup2(&fa, in[0], 0);
	posix_spawn_file_actions_addclose(&fa, in[1]);
	if (how == SPAWN_OVER_THE_PIPE)
		over_the_pipe(&fa, in[0]);
	else if (how == SPAWN_CLOEXEC_DEFAULT)
	{
		posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
		posix_spawn_file_actions_addinherit_np(&fa, 1);
		posix_spawn_file_actions_addinherit_np(&fa, 2);
	}
	int err = posix_spawn(&pid, path, &fa, &attr, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&attr);
	if (err != 0)
	{
		check(0, who);
		close(in[0]);
		close(in[1]);
		return;
	}
	close(in[0]);
	// as soon as posix_spawn() returns: XNU set the names before
	check_names(pid, exe, who);
	close(in[1]);
	int status = 0;
	waitpid(pid, &status, 0);
	// the child checked its own names too, and exits with its failures
	failures += WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static void spawned(const char* path, const char* exe, const char* who)
{
	spawned_how(path, exe, who, SPAWN_PLAIN);
}

static void write_file_mode(const char* path, const void* data, size_t len, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	write(fd, data, len);
	close(fd);
	chmod(path, mode);
}

static void write_file(const char* path, const void* data, size_t len)
{
	write_file_mode(path, data, len, 0755);
}

// posix_spawn() of an image that fails to activate, with file actions fa (or none): XNU's error, and no child left
static void spawn_fails_with(const char* path, int want, const char* what, posix_spawn_file_actions_t* fa)
{
	// "exit": should a copy of this test run after all, it exits at once
	char* argv[] = { (char*) path, "exit", NULL };
	pid_t pid = -1;
	int status, err = posix_spawn(&pid, path, fa, NULL, argv, environ);

	check(err == want, what);
	if (err != want)
		printf("# got %d (%s), want %d (%s)\n", err, strerror(err), want, strerror(want));
	check(pid == -1, "... gives no pid");
	errno = 0;
	check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "... and leaves no child");
}

static void spawn_fails(const char* path, int want, const char* what)
{
	spawn_fails_with(path, want, what, NULL);
}

// the main thread's name (Linux's comm) holding ')' and spaces: the fields after it in /proc/<pid>/stat still read
// right (a regression guard for the stat parser, which ended the name at the first ')')
static void odd_thread_name(const char* me)
{
	struct proc_bsdinfo bi;
	struct proc_bsdshortinfo si;
	struct proc_taskinfo ti;
	struct kinfo_proc kp;
	size_t kl = sizeof(kp);
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };

	pthread_setname_np("a) b (c");
	memset(&bi, 0, sizeof(bi));
	memset(&si, 0, sizeof(si));
	memset(&ti, 0, sizeof(ti));
	memset(&kp, 0, sizeof(kp));
	check(proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) == sizeof(bi) && bi.pbi_ppid == getppid(),
		"a main thread named \"a) b (c\": pbi_ppid is the parent");
	check(proc_pidinfo(getpid(), PROC_PIDT_SHORTBSDINFO, 0, &si, sizeof(si)) == sizeof(si) && si.pbsi_status == SRUN
			&& si.pbsi_ppid == getppid(),
		"... pbsi_status is SRUN and pbsi_ppid the parent");
	check(proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) == sizeof(ti) && ti.pti_threadnum >= 1,
		"... pti_threadnum counts this thread");
	check(sysctl(mib, 4, &kp, &kl, NULL, 0) == 0 && kp.kp_eproc.e_ppid == getppid(), "... kinfo_proc's e_ppid is the parent");
	check_names(getpid(), me, "... and the names stay the executable's");
	pthread_setname_np("");
}

int main(int argc, char** argv)
{
	char self[PATH_MAX], dir[PATH_MAX], path[PATH_MAX], script[PATH_MAX], interp[PATH_MAX], what[256];
	uint32_t n = sizeof(self);
	const char* me;

	// a copy that is to fail to run, and ran
	if (argc >= 2 && strcmp(argv[1], "exit") == 0)
		return 1;

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
			snprintf(what, sizeof(what), "a #! script, as its interpreter %s", interp_name);
			check_names(pid, interp_name, what);
			close(sin[1]);
			waitpid(pid, NULL, 0);
		}
		else
			check(0, "spawning a #! script");
		posix_spawn_file_actions_destroy(&fa);
	}

	// a symbolic link names the process by its target, as namei() leaves the last component
	{
		char link_path[PATH_MAX];
		snprintf(link_path, sizeof(link_path), "%s/proc_name_link", dir);
		snprintf(path, sizeof(path), "%s/%s", dir, names[0]);
		symlink(path, link_path);
		spawned(link_path, names[0], "a symbolic link to the 2-character name, spawned");
		unlink(link_path);
	}

	// a process darlingserver has no executable for (it exec'd a Linux program): Linux's comm
	{
		char* largv[] = { "/Volumes/SystemRoot/usr/bin/sleep", "30", NULL };
		struct proc_bsdinfo bi;
		if (posix_spawn(&pid, largv[0], NULL, NULL, largv, environ) == 0)
		{
			// posix_spawn() returns at the Linux exec, and darlingserver forgets the process when it sees it gone, a
			// moment later: until then it has the spawner's names. XNU has no such case; wait for that
			for (int i = 0; i < 200; i++)
			{
				memset(&bi, 0, sizeof(bi));
				if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) == sizeof(bi) && strcmp(bi.pbi_name, "sleep") == 0)
					break;
				usleep(50000);
			}
			check_names(pid, "sleep", "a Linux program, by Linux's comm");
			kill(pid, SIGKILL);
			waitpid(pid, NULL, 0);
		}
		else
			check(0, "spawning a Linux program");
	}

	// a zombie: found only when asked for (arg 1), and proc_name() (arg 0) doesn't
	{
		struct proc_bsdinfo bi;
		struct proc_bsdshortinfo si;
		char zname[2 * MAXCOMLEN + 1] = "";
		pid = fork();
		if (pid == 0)
			_exit(0);
		for (int i = 0; i < 200; i++)
		{
			memset(&si, 0, sizeof(si));
			if (proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 1, &si, sizeof(si)) == sizeof(si) && si.pbsi_status == SZOMB)
				break;
			usleep(50000);
		}
		check(si.pbsi_status == SZOMB, "a zombie, asked for: PROC_PIDT_SHORTBSDINFO finds it, SZOMB");
		errno = 0;
		check(proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) <= 0 && errno == ESRCH,
			"... not asked for: PROC_PIDTBSDINFO fails with ESRCH");
		check(proc_name(pid, zname, sizeof(zname)) == 0 && zname[0] == '\0', "... and proc_name() names nothing");
		waitpid(pid, NULL, 0);
	}

	// sysctl's p_nice is the process's nice value (a regression guard: it read another field of /proc/<pid>/stat)
	{
		int ready[2], go[2];
		char c;
		struct kinfo_proc kp;
		size_t kl = sizeof(kp);
		pipe(ready);
		pipe(go);
		pid = fork();
		if (pid == 0)
		{
			setpriority(PRIO_PROCESS, 0, 5);
			write(ready[1], "x", 1);
			read(go[0], &c, 1);
			_exit(0);
		}
		read(ready[0], &c, 1);
		int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
		memset(&kp, 0, sizeof(kp));
		check(sysctl(mib, 4, &kp, &kl, NULL, 0) == 0 && kp.kp_proc.p_nice == 5, "a process at nice 5: kinfo_proc's p_nice is 5");
		write(go[1], "x", 1);
		waitpid(pid, NULL, 0);
	}

	odd_thread_name(me);

	// file actions over the spawn's own pipe, which moves out of their way and still reaches mldr, and
	// POSIX_SPAWN_CLOEXEC_DEFAULT, which must still hand the pipe to mldr
	snprintf(path, sizeof(path), "%s/%s", dir, names[0]);
	spawned_how(path, names[0], "file actions over the spawn's pipe, spawned", SPAWN_OVER_THE_PIPE);
	spawned_how(path, names[0], "POSIX_SPAWN_CLOEXEC_DEFAULT, spawned", SPAWN_CLOEXEC_DEFAULT);

	// images that fail to activate (kern_exec.c). These architecture cases are Darling's on x86_64: an arm64 Mac runs
	// arm64. No architecture of a universal file runs here, nor a thin arm64 one (EBADARCH, as load_return_to_errno()
	// maps LOAD_BADARCH and exec_mach_imgact() fails grade_binary()); a byte-swapped Mach-O is recognized and not run
	// (EBADARCH); a Mach-O too short for its header (XNU reads zeros, which exec_mach_imgact() takes as no MH_EXECUTE:
	// ENOEXEC); a #! script whose interpreter is missing (ENOENT); an image or interpreter without an execute bit
	// (exec_check_permissions(): EACCES)
	{
		char bad[PATH_MAX];
		struct
		{
			struct fat_header h;
			struct fat_arch a;
			struct mach_header_64 m;
		} fat;
		memset(&fat, 0, sizeof(fat));
		fat.h.magic = OSSwapHostToBigInt32(FAT_MAGIC);
		fat.h.nfat_arch = OSSwapHostToBigInt32(1);
		fat.a.cputype = OSSwapHostToBigInt32(CPU_TYPE_ARM64);
		fat.a.offset = OSSwapHostToBigInt32(offsetof(__typeof__(fat), m));
		fat.a.size = OSSwapHostToBigInt32(sizeof(fat.m));
		fat.m.magic = MH_MAGIC_64;
		fat.m.cputype = CPU_TYPE_ARM64;
		fat.m.filetype = MH_EXECUTE;
		snprintf(bad, sizeof(bad), "%s/proc_name_arm64", dir);
		write_file(bad, &fat, sizeof(fat));
		spawn_fails(bad, EBADARCH, "a universal file with only arm64: posix_spawn() fails with EBADARCH");
		unlink(bad);

		write_file(bad, &fat.m, sizeof(fat.m));
		spawn_fails(bad, EBADARCH, "a thin arm64 Mach-O: posix_spawn() fails with EBADARCH");
		fat.m.magic = MH_CIGAM_64;
		fat.m.cputype = CPU_TYPE_X86_64;
		write_file(bad, &fat.m, sizeof(fat.m));
		spawn_fails(bad, EBADARCH, "a byte-swapped Mach-O: posix_spawn() fails with EBADARCH");
		unlink(bad);

		uint32_t short_macho[2] = { MH_MAGIC_64, CPU_TYPE_X86_64 };
		snprintf(bad, sizeof(bad), "%s/proc_name_short", dir);
		write_file(bad, short_macho, sizeof(short_macho));
		spawn_fails(bad, ENOEXEC, "a Mach-O too short for its header: posix_spawn() fails with ENOEXEC");
		// a regression guard: the pipe moved onto the stderr the actions closed, and mldr's message was read as the error
		{
			posix_spawn_file_actions_t fa;
			posix_spawn_file_actions_init(&fa);
			over_the_pipe(&fa, 0);
			spawn_fails_with(bad, ENOEXEC, "... also with file actions over the spawn's pipe", &fa);
			posix_spawn_file_actions_destroy(&fa);
		}
		unlink(bad);

		snprintf(bad, sizeof(bad), "%s/proc_name_no_interpreter", dir);
		write_file(bad, "#!/no/such/interpreter\n", 23);
		spawn_fails(bad, ENOENT, "a #! script whose interpreter is missing: posix_spawn() fails with ENOENT");
		unlink(bad);

		char noexec[PATH_MAX], line[PATH_MAX + 16];
		snprintf(noexec, sizeof(noexec), "%s/proc_name_noexec", dir);
		copyfile(self, noexec, NULL, COPYFILE_DATA);
		chmod(noexec, 0644);
		spawn_fails(noexec, EACCES, "a Mach-O without an execute bit: posix_spawn() fails with EACCES");
		pid = fork();
		if (pid == 0)
		{
			char* eargv[] = { noexec, "exit", NULL };
			execve(noexec, eargv, environ);
			_exit(errno);
		}
		status = 0;
		waitpid(pid, &status, 0);
		check(WIFEXITED(status) && WEXITSTATUS(status) == EACCES, "... and execve() returns EACCES to its caller");

		snprintf(bad, sizeof(bad), "%s/proc_name_noexec_interpreter", dir);
		int len = snprintf(line, sizeof(line), "#!%s exit\n", noexec);
		write_file(bad, line, len);
		spawn_fails(bad, EACCES, "a #! script whose interpreter has no execute bit: posix_spawn() fails with EACCES");
		unlink(bad);
		unlink(noexec);
	}

	// a spawner with stdout and stderr closed, whose pipe(2) answers descriptors 1 and 2: mldr's message for an image
	// that fails is not the error (a regression guard: it was read as one, and the child left unreaped)
	{
		char bad[PATH_MAX];
		int res[2];
		uint32_t short_macho[2] = { MH_MAGIC_64, CPU_TYPE_X86_64 };
		snprintf(bad, sizeof(bad), "%s/proc_name_short", dir);
		write_file(bad, short_macho, sizeof(short_macho));
		pipe(res);
		pid = fork();
		if (pid == 0)
		{
			char* bargv[] = { bad, NULL };
			pid_t child = -1, left;
			int r[2];
			close(1);
			close(2);
			r[0] = posix_spawn(&child, bad, NULL, NULL, bargv, environ);
			left = waitpid(-1, NULL, WNOHANG);
			r[1] = left == -1 && errno == ECHILD;
			write(res[1], r, sizeof(r));
			_exit(0);
		}
		int r[2] = { -1, 0 };
		close(res[1]);
		read(res[0], r, sizeof(r));
		close(res[0]);
		waitpid(pid, NULL, 0);
		check(r[0] == ENOEXEC, "stdout and stderr closed: a Mach-O too short for its header still fails with ENOEXEC");
		if (r[0] != ENOEXEC)
			printf("# got %d\n", r[0]);
		check(r[1], "... and leaves no child");
		unlink(bad);
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
