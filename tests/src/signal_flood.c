// A thread that takes a steady stream of handled signals still runs between them. Darling handles each signal
// through darlingserver (interrupt_enter, sigprocess, interrupt_exit); sigexc's debug output, compiled in, added about
// 76 more calls per signal (each a kprintf of a register), which made a signal cost 12 to 67 ms on a loaded host. At
// 50 signals a second, the thread then took the next one as soon as it returned from the last, and never ran again
// until they stopped: starting a thread hung that way while another process signalled.
//
// First, a handled signal the thread sends itself must cost at most MAX_PAIRS times a pair of Mach calls
// (mach_thread_self() and mach_port_deallocate(), each a darlingserver call too), whatever the host's load: the two
// are timed in alternating batches, and each takes its fastest batch, since a stall only ever adds time. Then a
// child signals this process every PERIOD_MS for FLOOD_MS. The main thread, the only one that takes the signal,
// counts in a loop meanwhile; the longest time between two of its iterations must stay under MAX_GAP_MS. That
// second check is what a program sees, but only on a loaded host does it fail without the fix. It uses only XNU's
// interface.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <mach/mach.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PERIOD_MS 20
#define FLOOD_MS 4000
#define MAX_GAP_MS 1000
#define BATCHES 20
#define ROUNDS 10
// A signal is 3 calls (interrupt_enter, sigprocess, interrupt_exit) and raise()'s pthread_kill: 2 pairs. Timed this
// way, it measures 1.2 to 4.1 pairs; with the debug output, 59 to 149. 15 leaves about 4 times either way.
#define MAX_PAIRS 15

static atomic_int handled;

static void on_signal(int sig)
{
	(void) sig;
	atomic_fetch_add(&handled, 1);
}

static int compare(const void* a, const void* b)
{
	double x = *(const double*) a, y = *(const double*) b;
	return (x > y) - (x < y);
}

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(void)
{
	struct sigaction sa = { .sa_handler = on_signal };
	pid_t parent = getpid(), flooder;
	int failed = 0, status;
	double pairs[BATCHES], signals[BATCHES];

	setvbuf(stdout, NULL, _IOLBF, 0);
	sigaction(SIGUSR1, &sa, NULL);

	for (int b = 0; b < BATCHES; b++)
	{
		double t0 = now_ms();
		for (int i = 0; i < ROUNDS; i++)
			mach_port_deallocate(mach_task_self(), mach_thread_self());
		double t1 = now_ms();
		for (int i = 0; i < ROUNDS; i++)
			raise(SIGUSR1);
		double t2 = now_ms();
		pairs[b] = (t1 - t0) / ROUNDS;
		signals[b] = (t2 - t1) / ROUNDS;
	}
	qsort(pairs, BATCHES, sizeof(double), compare);
	qsort(signals, BATCHES, sizeof(double), compare);
	double pair = pairs[0], signal = signals[0];
	printf("%s - a handled signal costs %.1f Mach call pairs (at most %d): %.3f ms vs %.3f ms\n",
		signal <= MAX_PAIRS * pair ? "ok" : "not ok", signal / pair, MAX_PAIRS, signal, pair);
	if (signal > MAX_PAIRS * pair)
		failed++;
	atomic_store(&handled, 0);

	flooder = fork();
	if (flooder == 0)
	{
		// on a fixed schedule, whatever each kill() and sleep costs
		double start = now_ms();
		for (int i = 1; i <= FLOOD_MS / PERIOD_MS; i++)
		{
			if (kill(parent, SIGUSR1) != 0)
				_exit(1);
			double wait = start + i * PERIOD_MS - now_ms();
			if (wait > 0)
				usleep((useconds_t) (wait * 1000));
		}
		_exit(0);
	}

	// until the flooder has finished
	double last = now_ms(), gap = 0;
	while (waitpid(flooder, &status, WNOHANG) == 0)
	{
		double t = now_ms();
		if (t - last > gap)
			gap = t - last;
		last = t;
	}

	printf("%s - a thread taking a signal every %d ms runs between them: longest gap %.0f ms (at most %d), %d "
		"signals\n", gap < MAX_GAP_MS ? "ok" : "not ok", PERIOD_MS, gap, MAX_GAP_MS, atomic_load(&handled));
	if (gap >= MAX_GAP_MS)
		failed++;
	// else the check above proves nothing
	printf("%s - the signals reach it\n", atomic_load(&handled) > 0 ? "ok" : "not ok");
	if (atomic_load(&handled) == 0)
		failed++;
	return failed;
}
