// posix_spawn() runs none of the caller's code in the new process, as on XNU, where the kernel creates it and
// activates its image (bsd/kern/kern_exec.c). Darling forks, and the child runs the file actions before its exec; it
// ran them with the caller's signal handlers, so a signal sent to the process group meanwhile (a terminal's SIGWINCH)
// ran a handler written for the caller in the child.
//
// - An open file action on a FIFO holds the child before its exec while this process sends SIGWINCH to its process
//   group. Each run of the handler writes a byte to a pipe and counts itself; more bytes than this process counted
//   ran in the child. (Not getpid(): the child may answer the caller's PID until its exec.)
// - The new image starts with the signal state XNU gives it: an ignored signal stays ignored, unless
//   POSIX_SPAWN_SETSIGDEF names it; the caller's mask, or POSIX_SPAWN_SETSIGMASK's. This test, run with "report",
//   tells its parent through its exit status.
// It uses only XNU's interface.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define REPORT_DFL 1      // SIGUSR2's action is SIG_DFL
#define REPORT_IGN 2      // SIGUSR2's action is SIG_IGN
#define REPORT_BLOCKED 4  // SIGURG is blocked

extern char** environ;

static int failures;
static int report_pipe[2];
static volatile sig_atomic_t handled;

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

static void on_winch(int sig)
{
	(void) sig;
	handled++;
	write(report_pipe[1], "c", 1);
}

static int report(void)
{
	struct sigaction sa;
	sigset_t mask;
	int r = 0;

	sigaction(SIGUSR2, NULL, &sa);
	if (sa.sa_handler == SIG_DFL)
		r |= REPORT_DFL;
	if (sa.sa_handler == SIG_IGN)
		r |= REPORT_IGN;
	sigprocmask(SIG_BLOCK, NULL, &mask);
	if (sigismember(&mask, SIGURG))
		r |= REPORT_BLOCKED;
	return r;
}

struct spawn {
	const char* path;
	posix_spawn_file_actions_t* actions;
	posix_spawnattr_t* attr;
	char** argv;
	pid_t pid;
	int err;
};

static void* spawner(void* p)
{
	struct spawn* s = p;
	s->err = posix_spawn(&s->pid, s->path, s->actions, s->attr, s->argv, environ);
	return NULL;
}

// spawns this test with "report" and the given attributes; returns its exit status, or -1
static int spawn_report(const char* self_path, posix_spawnattr_t* attr)
{
	char* argv[] = { (char*) self_path, "report", NULL };
	pid_t pid;
	int status;

	if (posix_spawn(&pid, self_path, NULL, attr, argv, environ) != 0 || waitpid(pid, &status, 0) != pid
			|| !WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

int main(int argc, char** argv)
{
	char fifo[64], detail[64];
	struct sigaction sa = { .sa_handler = on_winch };
	pthread_t thread;
	int status;

	if (argc > 1 && strcmp(argv[1], "report") == 0)
		return report();

	setvbuf(stdout, NULL, _IOLBF, 0);
	// a process group of its own, which kill(0, ...) signals
	setpgid(0, 0);
	snprintf(fifo, sizeof(fifo), "/private/var/tmp/spawn_signals.%d", getpid());
	unlink(fifo);
	if (mkfifo(fifo, 0600) != 0 || pipe(report_pipe) != 0)
	{
		printf("not ok - set up: %s\n", strerror(errno));
		return 1;
	}
	fcntl(report_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(report_pipe[1], F_SETFD, FD_CLOEXEC);
	sigaction(SIGWINCH, &sa, NULL);

	// the child blocks opening the FIFO, before its exec, until this process opens it to write
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 10, fifo, O_RDONLY, 0);
	char* true_argv[] = { "true", NULL };
	struct spawn s = { "/usr/bin/true", &actions, NULL, true_argv, 0, -1 };
	pthread_create(&thread, NULL, spawner, &s);

	// until the child exists (waitpid() then finds a child that hasn't exited), and a while after
	for (int i = 0; i < 500 && waitpid(-1, &status, WNOHANG) != 0; i++)
		usleep(10000);
	usleep(200000);
	for (int i = 0; i < 3; i++)
	{
		kill(0, SIGWINCH);
		usleep(50000);
	}
	int w = open(fifo, O_WRONLY);
	if (w >= 0)
		close(w);
	pthread_join(thread, NULL);
	int exited = s.err == 0 && waitpid(s.pid, &status, 0) == s.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	unlink(fifo);

	// SIGWINCH stays handled, blocked while it's counted
	sigset_t winch;
	char c;
	int bytes = 0;
	sigemptyset(&winch);
	sigaddset(&winch, SIGWINCH);
	sigprocmask(SIG_BLOCK, &winch, NULL);
	while (read(report_pipe[0], &c, 1) == 1)
		bytes++;
	snprintf(detail, sizeof(detail), "%d runs, %d of them here", bytes, (int) handled);
	check(bytes == handled, "no caller's handler runs in the spawned child before its exec", detail);
	snprintf(detail, sizeof(detail), "posix_spawn %d, status 0x%x", s.err, status);
	check(exited, "the child execs once the file action opens", detail);
	// else the checks above prove nothing
	check(handled > 0, "the signals reach this process", "none did");

	// the new image's signal state
	struct sigaction ign = { .sa_handler = SIG_IGN };
	sigset_t urg, empty;
	posix_spawnattr_t attr;
	int r;

	sigaction(SIGUSR2, &ign, NULL);
	sigemptyset(&urg);
	sigaddset(&urg, SIGURG);
	sigemptyset(&empty);
	sigprocmask(SIG_BLOCK, &urg, NULL);

	r = spawn_report(argv[0], NULL);
	snprintf(detail, sizeof(detail), "report %d", r);
	check(r >= 0 && (r & REPORT_IGN), "an ignored signal stays ignored", detail);
	check(r >= 0 && (r & REPORT_BLOCKED), "the new image starts with the caller's mask", detail);

	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
	sigset_t usr2;
	sigemptyset(&usr2);
	sigaddset(&usr2, SIGUSR2);
	posix_spawnattr_setsigdefault(&attr, &usr2);
	posix_spawnattr_setsigmask(&attr, &empty);
	r = spawn_report(argv[0], &attr);
	snprintf(detail, sizeof(detail), "report %d", r);
	check(r >= 0 && (r & REPORT_DFL), "POSIX_SPAWN_SETSIGDEF gives an ignored signal its default action", detail);
	check(r >= 0 && !(r & REPORT_BLOCKED), "POSIX_SPAWN_SETSIGMASK's mask replaces the caller's", detail);
	posix_spawnattr_destroy(&attr);

	return failures;
}
