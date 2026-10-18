// A new thread takes no signal until it has checked in with darlingserver on its own RPC socket. A daemon in
// dispatch_main() blocks signals on its main thread, so the process's signals go to its other threads, a thread just
// started too. Before its socket existed, a handler's first call (interrupt_enter) found none, and mldr aborted the
// process (__darling_thread_rpc_socket); between the socket and the checkin, darlingserver would register the thread
// from the handler's call instead of its checkin.
//
// The main thread starts THREADS short-lived threads one after another, with SIGUSR1 unblocked only while it does,
// so that each new thread starts with it unblocked, as a Darling workqueue thread does (it takes its creator's mask;
// XNU's starts with signals blocked). Meanwhile a child process sends this one two signals each time the last ones
// have been handled (or after WAIT_MS without): the main thread takes the first, and the second waits for a thread
// that can take it, often one just started. Each thread must start, the process must live, and the signals must be
// handled. It uses only XNU's interface.
//
// Whether a signal lands in a new thread's first instructions depends on scheduling, so a run without the fix fails
// only nearly always: with the thread-start mask disabled, 5 of 5 runs died, each before its 70th thread.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define THREADS 1000
#define WAIT_MS 100

// shared with the child
static atomic_int* handled;

static void on_signal(int sig)
{
	(void) sig;
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

int main(void)
{
	struct sigaction sa = { .sa_handler = on_signal };
	sigset_t usr1;
	pid_t parent = getpid(), flooder;
	int failed = 0, started = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	handled = mmap(NULL, sizeof(*handled), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	if (handled == MAP_FAILED)
	{
		printf("not ok - set up: mmap\n");
		return 1;
	}
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
			// the second waits while the main thread handles the first (in its handler, SIGUSR1 is blocked), for a
			// thread that starts meanwhile
			if (kill(parent, SIGUSR1) != 0 || kill(parent, SIGUSR1) != 0)
				_exit(0);
			for (int ms = 0; ms < WAIT_MS && atomic_load(handled) == before; ms++)
				sleep_ms(1);
		}
	}

	for (int i = 0; i < THREADS; i++)
	{
		pthread_t thread;

		pthread_sigmask(SIG_UNBLOCK, &usr1, NULL);
		int err = pthread_create(&thread, NULL, idle, NULL);
		pthread_sigmask(SIG_BLOCK, &usr1, NULL);
		if (err != 0)
			break;
		pthread_join(thread, NULL);
		started++;
	}
	kill(flooder, SIGKILL);
	waitpid(flooder, NULL, 0);

	printf("%s - %d of %d threads start while the process is signalled\n", started == THREADS ? "ok" : "not ok",
		started, THREADS);
	if (started != THREADS)
		failed++;
	// else the check above proves nothing
	printf("%s - the signals are handled (%d)\n", atomic_load(handled) > 0 ? "ok" : "not ok", atomic_load(handled));
	if (atomic_load(handled) == 0)
		failed++;
	return failed;
}
