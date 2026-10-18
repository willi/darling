// A signal that Linux gives a thread Darling didn't start reaches the app anyway, and a fault on such a thread ends
// the process as it would a Linux one. A host library loaded through elfcalls (a Vulkan driver, glibc's SIGEV_THREAD
// helper) starts threads with glibc's pthread_create: they never check in with darlingserver, but Darling's signal
// handler is every signal's, in every thread, and its first call (interrupt_enter) found no socket on them: mldr
// ended the process ("called darlingserver before its checkin").
//
// Each case runs in a process of its own (this program, spawned with the case's name):
// - "signal": a glibc thread starts first, with SIGUSR1 unblocked; then a Darwin thread; then the main thread blocks
//   SIGUSR1 and sends the process SIGUSR1s. Linux gives one to the first thread after the main one that takes it,
//   the glibc thread. The app's handler must run for each, and the glibc thread must block SIGUSR1 from then on.
// - "fault": a glibc thread faults (strlen(NULL)). The process must die of SIGSEGV, not SIGABRT.
//
// Only a Darling test: it reaches glibc through Darling's elfcalls, and reads Linux's /proc.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <dirent.h>
#include <elfcalls.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SIGNALS 20
#define WAIT_MS 5000

extern char** environ;
// libsystem_kernel's
extern void* elfcalls_get_pointer(void);

typedef unsigned long glibc_pthread_t;
typedef int (*glibc_pthread_create_f)(glibc_pthread_t*, const void*, void* (*)(void*), void*);

static atomic_int handled;

static void on_signal(int sig)
{
	(void) sig;
	atomic_fetch_add(&handled, 1);
}

static void* glibc_symbol(const char* name)
{
	struct elf_calls* calls = elfcalls_get_pointer();
	void* lib = calls->dlopen("libc.so.6");
	void* sym = (lib != NULL) ? calls->dlsym(lib, name) : NULL;

	// glibc before 2.34 has its threads in libpthread
	if (sym == NULL && (lib = calls->dlopen("libpthread.so.0")) != NULL)
		sym = calls->dlsym(lib, name);
	return sym;
}

// a thread of glibc's, running glibc's `routine`: no Darwin code may run on it
static int start_glibc_thread(void* routine, void* arg)
{
	glibc_pthread_create_f create = glibc_symbol("pthread_create");
	glibc_pthread_t thread;

	if (create == NULL || routine == NULL)
		return -1;
	return create(&thread, NULL, routine, arg);
}

// this process's threads' Linux IDs, up to `max`
static int tasks(int* tids, int max)
{
	DIR* dir = opendir("/proc/self/task");
	struct dirent* entry;
	int n = 0;

	if (dir == NULL)
		return 0;
	while ((entry = readdir(dir)) != NULL && n < max)
	{
		if (entry->d_name[0] != '.')
			tids[n++] = atoi(entry->d_name);
	}
	closedir(dir);
	return n;
}

// whether Linux thread `tid` blocks Linux signal `sig`; -1 if unknown
static int blocks(int tid, int sig)
{
	char path[64], buf[4096];
	int fd, len;

	snprintf(path, sizeof(path), "/proc/self/task/%d/status", tid);
	if ((fd = open(path, O_RDONLY)) < 0)
		return -1;
	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (len <= 0)
		return -1;
	buf[len] = '\0';

	char* line = strstr(buf, "SigBlk:");
	if (line == NULL)
		return -1;
	return (strtoull(line + 7, NULL, 16) >> (sig - 1)) & 1;
}

static void* wait_handled(void* arg)
{
	while (atomic_load(&handled) < SIGNALS)
		usleep(1000);
	return arg;
}

// the "signal" case; exits with its number of failed checks
static int signal_case(void)
{
	int before[64], after[64], nbefore, nafter, host_tid = 0, failed = 0;
	struct sigaction sa = { .sa_handler = on_signal };
	glibc_pthread_t main_thread;
	glibc_pthread_t (*self)(void) = glibc_symbol("pthread_self");
	pthread_t darwin;
	sigset_t usr1;

	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);

	if (self == NULL)
		return 100;
	main_thread = self();

	// the glibc thread joins the main one: it waits for ever, whatever signal it takes
	nbefore = tasks(before, 64);
	if (start_glibc_thread(glibc_symbol("pthread_join"), (void*) main_thread) != 0)
		return 100;
	for (int tries = 0; tries < 100 && host_tid == 0; tries++)
	{
		nafter = tasks(after, 64);
		for (int i = 0; i < nafter && host_tid == 0; i++)
		{
			int known = 0;
			for (int j = 0; j < nbefore; j++)
				known |= after[i] == before[j];
			if (!known)
				host_tid = after[i];
		}
		if (host_tid == 0)
			usleep(10000);
	}

	pthread_create(&darwin, NULL, wait_handled, NULL);
	sigemptyset(&usr1);
	sigaddset(&usr1, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &usr1, NULL);

	for (int i = 0; i < SIGNALS; i++)
	{
		kill(getpid(), SIGUSR1);
		for (int ms = 0; ms < WAIT_MS && atomic_load(&handled) <= i; ms++)
			usleep(1000);
	}

	printf("%s - %d signals to the process reach the app with a glibc thread in it: %d handled\n",
		atomic_load(&handled) == SIGNALS ? "ok" : "not ok", SIGNALS, atomic_load(&handled));
	if (atomic_load(&handled) != SIGNALS)
		failed++;

	// Linux numbers SIGUSR1 10 (XNU 30)
	int blocked = (host_tid != 0) ? blocks(host_tid, 10) : -1;
	printf("%s - the glibc thread took one, and blocks SIGUSR1 since%s\n", blocked == 1 ? "ok" : "not ok",
		(blocked == 1) ? "" : (blocked == 0) ? ": it doesn't (it took none?)" : ": its mask can't be read");
	if (blocked != 1)
		failed++;

	pthread_join(darwin, NULL);
	return failed;
}

// the "fault" case: the process dies of the glibc thread's fault
static int fault_case(void)
{
	if (start_glibc_thread(glibc_symbol("strlen"), NULL) != 0)
		return 100;
	sleep(WAIT_MS / 1000);
	return 100;
}

// runs this program's case `name`; answers its wait status, or -1
static int spawn_case(const char* name)
{
	char path[1024];
	uint32_t size = sizeof(path);
	char* argv[] = { path, (char*) name, NULL };
	pid_t pid;
	int status;

	fflush(stdout);
	if (_NSGetExecutablePath(path, &size) != 0 || posix_spawn(&pid, path, NULL, NULL, argv, environ) != 0)
		return -1;
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return status;
}

int main(int argc, char** argv)
{
	int failed = 0, status;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 2 && strcmp(argv[1], "signal") == 0)
		return signal_case();
	if (argc == 2 && strcmp(argv[1], "fault") == 0)
		return fault_case();

	status = spawn_case("signal");
	if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) == 100)
	{
		if (status != -1 && WIFSIGNALED(status))
			printf("not ok - a signal on a glibc thread: the process died of signal %d\n", WTERMSIG(status));
		else
			printf("not ok - set up: no glibc thread, or the case couldn't run\n");
		failed++;
	}
	else
		failed += WEXITSTATUS(status);

	status = spawn_case("fault");
	int died = status != -1 && WIFSIGNALED(status);
	printf("%s - a fault on a glibc thread ends the process with that fault's signal: %s %d\n",
		(died && WTERMSIG(status) == SIGSEGV) ? "ok" : "not ok", died ? "signal" : "exit status",
		died ? WTERMSIG(status) : (status == -1) ? -1 : WEXITSTATUS(status));
	if (!died || WTERMSIG(status) != SIGSEGV)
		failed++;
	return failed;
}
