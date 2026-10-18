// A kprintf (libsystem_kernel's __simple_kprintf, Darling's debug log to darlingserver) costs no darlingserver call
// when darlingserver doesn't log it, as at its default log level. Each was a round trip whose string darlingserver then
// dropped; on a signal path, 76 of them per signal made a livelock.
//
// The fastest of BATCHES batches of ROUNDS kprintfs must cost at most MAX_PAIRS of a pair of Mach calls
// (mach_thread_self() and mach_port_deallocate(), darlingserver calls too), timed in alternating batches; a kprintf
// that made its call measures about half a pair. Run it at darlingserver's default log level: with DSERVER_LOG_LEVEL
// at info or below, or "kprintf" in DSERVER_LOG_CATEGORIES, the calls are made, and the check fails.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BATCHES 20
#define ROUNDS 10
#define MAX_PAIRS 0.1

// libsystem_kernel's; not in any header
extern void __simple_kprintf(const char* format, ...);

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
	double pairs[BATCHES], kprintfs[BATCHES];

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int b = 0; b < BATCHES; b++)
	{
		double t0 = now_ms();
		for (int i = 0; i < ROUNDS; i++)
			mach_port_deallocate(mach_task_self(), mach_thread_self());
		double t1 = now_ms();
		for (int i = 0; i < ROUNDS; i++)
			__simple_kprintf("kprintf_cost: %d.%d\n", b, i);
		double t2 = now_ms();
		pairs[b] = (t1 - t0) / ROUNDS;
		kprintfs[b] = (t2 - t1) / ROUNDS;
	}
	qsort(pairs, BATCHES, sizeof(double), compare);
	qsort(kprintfs, BATCHES, sizeof(double), compare);

	double pair = pairs[0], kprintf = kprintfs[0];
	int ok = kprintf <= MAX_PAIRS * pair;
	printf("%s - an unlogged kprintf costs %.3f Mach call pairs (at most %.1f): %.4f ms vs %.3f ms\n",
		ok ? "ok" : "not ok", kprintf / pair, MAX_PAIRS, kprintf, pair);
	return !ok;
}
