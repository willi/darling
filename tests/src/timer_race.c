// Many threads arming short timed waits at once. darlingserver keeps one timerfd for all of them; a timer enqueued
// while another worker thread re-armed the timerfd after expiring the queue used to be lost, leaving the timerfd
// disarmed with waits pending: every later sleep() or semaphore_timedwait() in every process then hung (launchd_sim's
// device stopped booting). It only relies on XNU's interface, so it runs on macOS too.
//
// The watchdog busy-waits on mach_absolute_time(), since a lost timer would hang its own sleep too.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define THREAD_COUNT 16
#define ITERATIONS 300
#define STALL_MS 5000

static atomic_long progress;
static atomic_int done_threads;
static mach_timebase_info_data_t timebase;

static uint64_t now_ms(void)
{
	return mach_absolute_time() * timebase.numer / timebase.denom / 1000000;
}

static void* waiter(void* arg)
{
	semaphore_t sem;
	unsigned seed = (unsigned) (uintptr_t) arg;

	if (semaphore_create(mach_task_self(), &sem, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS)
		abort();

	for (int i = 0; i < ITERATIONS; i++)
	{
		unsigned us = rand_r(&seed) % 2000;

		// alternate the two kinds of timed wait; both time out, as nothing signals the semaphore
		if (i & 1)
			usleep(us);
		else
		{
			mach_timespec_t timeout = { 0, us * 1000 };
			semaphore_timedwait(sem, timeout);
		}
		atomic_fetch_add(&progress, 1);
	}

	semaphore_destroy(mach_task_self(), sem);
	atomic_fetch_add(&done_threads, 1);
	return NULL;
}

int main(void)
{
	pthread_t threads[THREAD_COUNT];
	long last_progress = -1;
	uint64_t last_change;

	mach_timebase_info(&timebase);
	for (int i = 0; i < THREAD_COUNT; i++)
		pthread_create(&threads[i], NULL, waiter, (void*) (uintptr_t) (i + 1));

	last_change = now_ms();
	while (atomic_load(&done_threads) < THREAD_COUNT)
	{
		long p = atomic_load(&progress);

		if (p != last_progress)
		{
			last_progress = p;
			last_change = now_ms();
		}
		else if (now_ms() - last_change > STALL_MS)
		{
			printf("not ok - timed waits all complete: no progress for %d ms after %ld of %d waits\n", STALL_MS, p,
				THREAD_COUNT * ITERATIONS);
			fflush(stdout);
			// the stuck threads can't be joined
			_exit(1);
		}
		sched_yield();
	}

	for (int i = 0; i < THREAD_COUNT; i++)
		pthread_join(threads[i], NULL);
	printf("ok - timed waits all complete\n");

	// and the timer still works for a fresh wait afterwards
	uint64_t start = now_ms();
	usleep(100000);
	uint64_t elapsed = now_ms() - start;
	if (elapsed >= 100 && elapsed < 2000)
		printf("ok - later sleep fires on time\n");
	else
	{
		printf("not ok - later sleep fires on time: took %llu ms\n", (unsigned long long) elapsed);
		return 1;
	}
	return 0;
}
