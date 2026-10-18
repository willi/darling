// A new thread's signal handlers run on its alternate signal stack from its first signal on, and the stack goes when
// the thread does.
//
// - First signal: Darling gave a thread its alternate stack in libpthread's entry wrapper, after mldr had unblocked
//   its signals: a handler that ran in between ran on the thread's own stack, which a stack overflow's SIGSEGV can't
//   run on. As in thread_start_signals, the main thread starts FIRST_THREADS threads one after another, with SIGUSR1
//   unblocked only while it does, so that each starts with it unblocked; a child process sends this one two signals
//   each time the last ones have been handled: the main thread takes the first, and the second waits for a thread
//   that can take it, often one just started, as mldr unblocks its signals. Every handler must run on an alternate
//   stack. (A pthread_kill() right after pthread_create() comes too late: it's a darlingserver call.)
// - The stack's end: the alternate stack was never freed, 52 KiB and two mappings per thread, so a process that
//   starts threads for long enough runs out of mappings (vm.max_map_count) and every mmap() fails. THREADS threads
//   start and end one after another, and the process may gain far fewer mappings than they would have leaked (its
//   size moves by more than a leak would, with glibc's cache of 8 MiB thread stacks).
//
// It uses only XNU's interface, plus Linux's /proc for the process's mappings.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FIRST_THREADS 1000
#define WAIT_MS 100
#define THREADS 500
// what THREADS leaked alternate stacks would be: a guard page and a stack each
#define LEAK_MAPPINGS (THREADS * 2)

// shared with the child
static atomic_int* handled;
static atomic_int off_stack;

static void on_signal(int sig)
{
	stack_t ss;

	(void) sig;
	if (sigaltstack(NULL, &ss) != 0 || !(ss.ss_flags & SS_ONSTACK))
		atomic_fetch_add(&off_stack, 1);
	atomic_fetch_add(handled, 1);
}

static void* idle(void* arg)
{
	return arg;
}

static void sleep_ms(long ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000 };
	nanosleep(&t, NULL);
}

// the process's number of mappings, from Linux's /proc; 0 if unknown
static int mappings(void)
{
	char buf[4096];
	int fd = open("/proc/self/maps", O_RDONLY), n = 0;
	ssize_t len;

	if (fd < 0)
		return 0;
	while ((len = read(fd, buf, sizeof(buf))) > 0)
	{
		for (ssize_t i = 0; i < len; i++)
			n += buf[i] == '\n';
	}
	close(fd);
	return n;
}

// starts and joins `n` threads, one after another, with `unblock` unblocked while each is created (if not NULL);
// answers how many started
static int churn(int n, const sigset_t* unblock)
{
	int started = 0;

	for (int i = 0; i < n; i++)
	{
		pthread_t thread;

		if (unblock != NULL)
			pthread_sigmask(SIG_UNBLOCK, unblock, NULL);
		int err = pthread_create(&thread, NULL, idle, NULL);
		if (unblock != NULL)
			pthread_sigmask(SIG_BLOCK, unblock, NULL);
		if (err != 0)
			break;
		pthread_join(thread, NULL);
		started++;
	}
	return started;
}

int main(void)
{
	int failed = 0;
	struct sigaction sa = { .sa_handler = on_signal };
	sigset_t usr1;
	pid_t parent = getpid(), flooder;

	setvbuf(stdout, NULL, _IOLBF, 0);
	handled = mmap(NULL, sizeof(*handled), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	if (handled == MAP_FAILED)
	{
		printf("not ok - set up: mmap\n");
		return 1;
	}
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	sigemptyset(&usr1);
	sigaddset(&usr1, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &usr1, NULL);

	flooder = fork();
	if (flooder == 0)
	{
		for (;;)
		{
			int before = atomic_load(handled);
			if (kill(parent, SIGUSR1) != 0 || kill(parent, SIGUSR1) != 0)
				_exit(0);
			for (int ms = 0; ms < WAIT_MS && atomic_load(handled) == before; ms++)
				sleep_ms(1);
		}
	}
	int started = churn(FIRST_THREADS, &usr1);
	kill(flooder, SIGKILL);
	waitpid(flooder, NULL, 0);

	int ok = started == FIRST_THREADS && atomic_load(handled) > 0 && atomic_load(&off_stack) == 0;
	printf("%s - a new thread's first signal runs on its alternate stack: %d of %d handled off it, %d of %d threads\n",
		ok ? "ok" : "not ok", atomic_load(&off_stack), atomic_load(handled), started, FIRST_THREADS);
	if (!ok)
		failed++;

	// the first threads fill what's cached: libpthread's and glibc's stacks
	churn(20, NULL);
	int before = mappings();
	started = churn(THREADS, NULL);
	int grown = mappings() - before;
	ok = before != 0 && started == THREADS && grown < LEAK_MAPPINGS / 4;

	printf("%s - %d threads that ended leave no alternate stacks behind: %d more mappings (leaked, %d)\n",
		ok ? "ok" : "not ok", started, grown, LEAK_MAPPINGS);
	if (!ok)
		failed++;

	return failed;
}
