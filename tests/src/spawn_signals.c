// posix_spawn() runs none of the caller's code in the new process, as on XNU, where the kernel creates it and
// activates its image (bsd/kern/kern_exec.c), and the new image starts with the signal state XNU's exec gives it
// (execsigs(), bsd/kern/kern_sig.c). Darling forks, and the child runs the file actions before its exec; it ran them
// with the caller's signal handlers, so a signal sent to the process group meanwhile (a terminal's SIGWINCH) ran a
// handler written for the caller in the child.
//
// - An open file action on a FIFO holds the child before its exec while this process sends SIGWINCH to its process
//   group. Each run of the handler writes a byte to a pipe and counts itself; more bytes than this process counted
//   ran in the child. (Not getpid(): the child may answer the caller's PID until its exec.)
// - The same with SIGUSR1, which this process handles: the child, with its default action, must end at once, while
//   its open still waits, as XNU's interruptible sleep would end the spawn. (That a signal reached it at all is what
//   makes the first check mean something.)
// - The new image starts with the signal state XNU gives it: an ignored signal stays ignored (SIGCHLD too, which
//   Linux keeps itself), unless POSIX_SPAWN_SETSIGDEF names it; the caller's mask, or POSIX_SPAWN_SETSIGMASK's. This
//   test, run with "report", writes what it has to a pipe on REPORT_FD.
// - An exec that fails leaves this process's signals as they were: one that fails with E2BIG, and a
//   POSIX_SPAWN_SETEXEC spawn with SETSIGDEF and SETSIGMASK that fails.
// - A process that inherited SIGTTOU ignored, in a background process group of its terminal (a pty of a session of
//   its own), sets the terminal's attributes, as the kernel lets it when SIGTTOU is ignored, instead of being sent
//   SIGTTOU again and again. Skipped where no pty can be made.
// It uses only XNU's interface.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define REPORT_DFL 1       // SIGUSR2's action is SIG_DFL
#define REPORT_IGN 2       // SIGUSR2's action is SIG_IGN
#define REPORT_BLOCKED 4   // SIGURG is blocked
#define REPORT_CHLD_IGN 8  // SIGCHLD's action is SIG_IGN
#define REPORT_FD 3
#define WAIT_MS 10000

extern char** environ;

static char self_path[1024];
static int failures;
static int report_pipe[2];
static volatile sig_atomic_t handled;
static volatile sig_atomic_t vtalrm_handled;

static void check(int ok, const char* what, const char* detail)
{
	if (ok)
		printf("ok - %s\n", what);
	else
	{
		printf("not ok - %s: %s\n", what, detail);
		failures++;
	}
}

static void sleep_ms(long ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000 };
	nanosleep(&t, NULL);
}

// SIGWINCH and SIGUSR1's: wherever it runs, a byte in the pipe; only here, a count
static void on_signal(int sig)
{
	(void) sig;
	handled++;
	write(report_pipe[1], "c", 1);
}

static void on_vtalrm(int sig)
{
	(void) sig;
	vtalrm_handled++;
}

// "report": this image's signal state, to REPORT_FD
static int report(void)
{
	struct sigaction sa, chld;
	sigset_t mask;
	int r = 0;

	sigaction(SIGUSR2, NULL, &sa);
	if (sa.sa_handler == SIG_DFL)
		r |= REPORT_DFL;
	if (sa.sa_handler == SIG_IGN)
		r |= REPORT_IGN;
	sigaction(SIGCHLD, NULL, &chld);
	if (chld.sa_handler == SIG_IGN)
		r |= REPORT_CHLD_IGN;
	sigprocmask(SIG_BLOCK, NULL, &mask);
	if (sigismember(&mask, SIGURG))
		r |= REPORT_BLOCKED;
	write(REPORT_FD, &r, sizeof(r));
	return 0;
}

// "ttou": with SIGTTOU ignored, in a background process group of the terminal on stdin; 0 if tcsetattr() returns
static int ttou_report(void)
{
	struct termios t;

	alarm(5);
	if (tcgetattr(0, &t) != 0)
		return 2;
	return tcsetattr(0, TCSANOW, &t) == 0 ? 0 : 3;
}

struct spawn {
	const char* path;
	posix_spawn_file_actions_t* actions;
	posix_spawnattr_t* attr;
	char** argv;
	pid_t pid;
	int err;
	atomic_int done;
};

static void* spawner(void* p)
{
	struct spawn* s = p;
	s->err = posix_spawn(&s->pid, s->path, s->actions, s->attr, s->argv, environ);
	atomic_store(&s->done, 1);
	return NULL;
}

// spawns this test with "report" and the given attributes; answers its report, or -1
static int spawn_report(posix_spawnattr_t* attr)
{
	char* argv[] = { self_path, "report", NULL };
	posix_spawn_file_actions_t actions;
	int p[2], r = -1, status;
	pid_t pid;

	if (pipe(p) != 0)
		return -1;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, p[1], REPORT_FD);
	int err = posix_spawn(&pid, self_path, &actions, attr, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(p[1]);
	if (err == 0 && read(p[0], &r, sizeof(r)) != sizeof(r))
		r = -1;
	close(p[0]);
	// with SIGCHLD ignored, Linux has reaped it already
	if (err == 0)
		waitpid(pid, &status, 0);
	return (err == 0) ? r : -1;
}

// opens the FIFO's write end once its reader (the child's file action) has opened it; -1 after `ms`
static int open_fifo_writer(const char* fifo, int ms)
{
	for (int waited = 0; waited < ms; waited += 10)
	{
		int fd = open(fifo, O_WRONLY | O_NONBLOCK);
		if (fd >= 0 || errno != ENXIO)
			return fd;
		sleep_ms(10);
	}
	return -1;
}

// starts spawning /usr/bin/true with an open of `fifo` as a file action, and waits until the child exists
static void start_fifo_spawn(struct spawn* s, pthread_t* thread, posix_spawn_file_actions_t* actions, const char* fifo)
{
	static char* true_argv[] = { "true", NULL };
	int status;

	posix_spawn_file_actions_init(actions);
	posix_spawn_file_actions_addopen(actions, 10, fifo, O_RDONLY, 0);
	*s = (struct spawn) { "/usr/bin/true", actions, NULL, true_argv, 0, -1, 0 };
	pthread_create(thread, NULL, spawner, s);

	// until the child exists (waitpid() then finds a child that hasn't exited), and a while after
	for (int i = 0; i < 500 && waitpid(-1, &status, WNOHANG) != 0; i++)
		sleep_ms(10);
	sleep_ms(200);
}

// the SIGTTOU case, in a child of its own: 0 ok, 10 no pty, else failed
static int ttou_case(void)
{
	pid_t a = fork(), child;
	int status;

	if (a < 0)
		return 1;
	if (a == 0)
	{
		alarm(30);
		if (setsid() < 0)
			_exit(10);
		int m = posix_openpt(O_RDWR | O_NOCTTY);
		char* name = (m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0) ? ptsname(m) : NULL;
		// the session's controlling terminal, with this process's group in the foreground
		int s = name ? open(name, O_RDWR) : -1;
		if (s < 0 || tcgetpgrp(s) != getpgrp())
			_exit(10);
		signal(SIGTTOU, SIG_IGN);

		posix_spawn_file_actions_t actions;
		posix_spawnattr_t attr;
		char* argv[] = { self_path, "ttou", NULL };
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_adddup2(&actions, s, 0);
		posix_spawnattr_init(&attr);
		posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
		posix_spawnattr_setpgroup(&attr, 0);
		if (posix_spawn(&child, self_path, &actions, &attr, argv, environ) != 0)
			_exit(11);
		for (int ms = 0; ms < WAIT_MS; ms += 10)
		{
			if (waitpid(child, &status, WNOHANG) == child)
				_exit(WIFEXITED(status) ? WEXITSTATUS(status) : 13);
			sleep_ms(10);
		}
		kill(child, SIGKILL);
		waitpid(child, &status, 0);
		_exit(12);
	}
	if (waitpid(a, &status, 0) != a || !WIFEXITED(status))
		return 1;
	return WEXITSTATUS(status);
}

int main(int argc, char** argv)
{
	char fifo[64], fifo2[64], detail[96];
	struct sigaction sa = { .sa_handler = on_signal };
	posix_spawn_file_actions_t actions, actions2;
	struct spawn s, s2;
	pthread_t thread, thread2;
	uint32_t path_size = sizeof(self_path);
	int status = 0;

	if (argc > 1 && strcmp(argv[1], "report") == 0)
		return report();
	if (argc > 1 && strcmp(argv[1], "ttou") == 0)
		return ttou_report();
	if (argc > 1)
		return 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	// a backstop: nothing below may hang the suite
	alarm(120);
	if (_NSGetExecutablePath(self_path, &path_size) != 0)
	{
		printf("not ok - set up: no executable path\n");
		return 1;
	}

	int ttou = ttou_case();
	if (ttou == 10)
		printf("ok - a background process that inherited SIGTTOU ignored sets its terminal # skip no pty\n");
	else
	{
		snprintf(detail, sizeof(detail), "%s (%d)", ttou == 12 ? "it never returned" : ttou == 13
			? "it was killed (its alarm: it kept taking SIGTTOU)" : "it failed", ttou);
		check(ttou == 0, "a background process that inherited SIGTTOU ignored sets its terminal", detail);
	}

	// a process group of its own, which kill(0, ...) signals
	setpgid(0, 0);
	snprintf(fifo, sizeof(fifo), "/private/var/tmp/spawn_signals.%d", getpid());
	snprintf(fifo2, sizeof(fifo2), "/private/var/tmp/spawn_signals.%d.2", getpid());
	unlink(fifo);
	unlink(fifo2);
	if (mkfifo(fifo, 0600) != 0 || mkfifo(fifo2, 0600) != 0 || pipe(report_pipe) != 0)
	{
		printf("not ok - set up: %s\n", strerror(errno));
		return 1;
	}
	fcntl(report_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(report_pipe[1], F_SETFD, FD_CLOEXEC);
	sigemptyset(&sa.sa_mask);
	sigaction(SIGWINCH, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);

	// SIGWINCH while the child's open waits: the child ignores it (its default) and execs once the FIFO opens
	start_fifo_spawn(&s, &thread, &actions, fifo);
	for (int i = 0; i < 3; i++)
	{
		kill(0, SIGWINCH);
		sleep_ms(50);
	}
	int w = open_fifo_writer(fifo, WAIT_MS);
	if (w >= 0)
		close(w);
	pthread_join(thread, NULL);
	int exited = s.err == 0 && waitpid(s.pid, &status, 0) == s.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	unlink(fifo);

	// SIGUSR1 while the child's open waits: its default action ends the child before the FIFO is opened
	start_fifo_spawn(&s2, &thread2, &actions2, fifo2);
	kill(0, SIGUSR1);
	for (int ms = 0; ms < WAIT_MS && !atomic_load(&s2.done); ms += 10)
		sleep_ms(10);
	int ended = atomic_load(&s2.done);
	int status2 = 0;
	int killed = ended && (s2.err != 0 || (waitpid(s2.pid, &status2, 0) == s2.pid && WIFSIGNALED(status2)
		&& WTERMSIG(status2) == SIGUSR1));
	if (!ended)
	{
		// it waits still: let it go
		w = open_fifo_writer(fifo2, WAIT_MS);
		if (w >= 0)
			close(w);
		pthread_join(thread2, NULL);
		if (s2.err == 0)
			waitpid(s2.pid, &status2, 0);
	}
	else
		pthread_join(thread2, NULL);
	unlink(fifo2);

	// SIGWINCH and SIGUSR1 stay handled, blocked while the runs are counted
	sigset_t counted;
	char c;
	int bytes = 0;
	sigemptyset(&counted);
	sigaddset(&counted, SIGWINCH);
	sigaddset(&counted, SIGUSR1);
	sigprocmask(SIG_BLOCK, &counted, NULL);
	while (read(report_pipe[0], &c, 1) == 1)
		bytes++;
	snprintf(detail, sizeof(detail), "%d runs, %d of them here", bytes, (int) handled);
	check(bytes == handled, "no caller's handler runs in the spawned child before its exec", detail);
	snprintf(detail, sizeof(detail), "posix_spawn %d, status 0x%x", s.err, s.err == 0 ? status : 0);
	check(exited, "the child execs once the file action opens", detail);
	// else the checks above prove nothing
	check(handled > 0, "the signals reach this process", "none did");
	snprintf(detail, sizeof(detail), "%s (posix_spawn %d, status 0x%x)",
		ended ? "it didn't die of SIGUSR1" : "it still waited for the FIFO", s2.err, status2);
	check(killed, "a signal while a file action waits has its default action in the child", detail);
	sigprocmask(SIG_UNBLOCK, &counted, NULL);

	// the new image's signal state
	struct sigaction ign = { .sa_handler = SIG_IGN }, dfl = { .sa_handler = SIG_DFL };
	sigset_t urg, empty;
	posix_spawnattr_t attr;
	int r;

	sigaction(SIGUSR2, &ign, NULL);
	sigemptyset(&urg);
	sigaddset(&urg, SIGURG);
	sigemptyset(&empty);
	sigprocmask(SIG_BLOCK, &urg, NULL);

	r = spawn_report(NULL);
	snprintf(detail, sizeof(detail), "report %d", r);
	check(r >= 0 && (r & REPORT_IGN), "an ignored signal stays ignored", detail);
	check(r >= 0 && (r & REPORT_BLOCKED), "the new image starts with the caller's mask", detail);

	sigaction(SIGCHLD, &ign, NULL);
	r = spawn_report(NULL);
	sigaction(SIGCHLD, &dfl, NULL);
	snprintf(detail, sizeof(detail), "report %d", r);
	check(r >= 0 && (r & REPORT_CHLD_IGN), "an ignored SIGCHLD stays ignored, and says so", detail);

	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
	sigset_t usr2;
	sigemptyset(&usr2);
	sigaddset(&usr2, SIGUSR2);
	posix_spawnattr_setsigdefault(&attr, &usr2);
	posix_spawnattr_setsigmask(&attr, &empty);
	r = spawn_report(&attr);
	snprintf(detail, sizeof(detail), "report %d", r);
	check(r >= 0 && (r & REPORT_DFL), "POSIX_SPAWN_SETSIGDEF gives an ignored signal its default action", detail);
	check(r >= 0 && !(r & REPORT_BLOCKED), "POSIX_SPAWN_SETSIGMASK's mask replaces the caller's", detail);
	posix_spawnattr_destroy(&attr);

	// an exec that fails (E2BIG: one argument over Linux's 128 KiB) leaves an ignored signal as this process has it
	struct sigaction vt = { .sa_handler = on_vtalrm };
	size_t big_len = 200 << 10;
	char* big = malloc(big_len + 1);
	int exec_errno = 0;
	sigemptyset(&vt.sa_mask);
	sigaction(SIGVTALRM, &ign, NULL);
	if (big != NULL)
	{
		memset(big, 'x', big_len);
		big[big_len] = '\0';
		char* av[] = { self_path, "never", big, NULL };
		execve(self_path, av, environ);
		exec_errno = errno;
		free(big);
	}
	sigaction(SIGVTALRM, &vt, NULL);
	raise(SIGVTALRM);
	snprintf(detail, sizeof(detail), "execve: %s; the handler ran %d times", strerror(exec_errno), (int) vtalrm_handled);
	check(exec_errno == E2BIG && vtalrm_handled == 1, "after an exec that fails, a signal's handler runs", detail);

	// POSIX_SPAWN_SETEXEC that fails: SETSIGDEF and SETSIGMASK are undone
	char* av[] = { self_path, "never", NULL };
	struct sigaction cur;
	sigset_t mask;
	pid_t pid;
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
	sigset_t usr1;
	sigemptyset(&usr1);
	sigaddset(&usr1, SIGUSR1);
	posix_spawnattr_setsigdefault(&attr, &usr1);
	posix_spawnattr_setsigmask(&attr, &empty);
	int err = posix_spawn(&pid, "/private/var/tmp/spawn_signals.none", NULL, &attr, av, environ);
	posix_spawnattr_destroy(&attr);
	sigaction(SIGUSR1, NULL, &cur);
	sigprocmask(SIG_BLOCK, NULL, &mask);
	snprintf(detail, sizeof(detail), "posix_spawn %d; handler %s; SIGURG %s", err,
		cur.sa_handler == on_signal ? "kept" : "gone", sigismember(&mask, SIGURG) ? "blocked" : "unblocked");
	check(err != 0 && cur.sa_handler == on_signal && sigismember(&mask, SIGURG),
		"a POSIX_SPAWN_SETEXEC spawn that fails leaves the caller's handlers and mask", detail);

	return failures;
}
