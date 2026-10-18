// A handled signal costs a few darlingserver calls, not dozens. Darling handles each signal through darlingserver
// (interrupt_enter, sigprocess, interrupt_exit); sigexc's debug output, compiled in, added about 76 more calls per
// signal (each a kprintf of a register), which made a signal cost 12 to 67 ms on a loaded host. At 50 signals a
// second, a thread then took the next one as soon as it returned from the last, and never ran again until they
// stopped: starting a thread hung that way while another process signalled.
//
// A handled signal the thread sends itself must cost at most MAX_PAIRS times a pair of Mach calls (mach_thread_self()
// and mach_port_deallocate(), each a darlingserver call too), whatever the host's load: the two are timed in
// alternating batches, and each takes its fastest batch, since a stall only ever adds time. That cost is what decides
// whether a flood starves the thread; whether one did also depends on how the host schedules the sender, so there's
// no check of that. It uses only XNU's interface.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <mach/mach.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define BATCHES 20
#define ROUNDS 10
// A signal is 3 calls (interrupt_enter, sigprocess, interrupt_exit) and raise()'s pthread_kill: 2 pairs. Timed this
// way, it measures 1.2 to 4.1 pairs; with the debug output, 59 to 149. 15 leaves about 4 times either way.
#define MAX_PAIRS 15

static volatile sig_atomic_t handled;

static void on_signal(int sig)
{
	(void) sig;
	handled++;
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
	int failed = 0;
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
	// else the check above proves nothing
	printf("%s - each signal reaches the handler (%d of %d)\n", handled == BATCHES * ROUNDS ? "ok" : "not ok",
		(int) handled, BATCHES * ROUNDS);
	if (handled != BATCHES * ROUNDS)
		failed++;
	return failed;
}
