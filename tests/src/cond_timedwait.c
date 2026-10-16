// A timed wait on a condition variable that times out answers ETIMEDOUT, and the condition variable works on. The
// server answers a psynch wait's error with XNU's bits in it (316: ECVCLEARED | ETIMEDOUT), and Darling returned that
// as the wait's result instead of as an error: the wait answered 0, libpthread folded 316 into the condition
// variable's sequence words, and after enough timeouts a wait failed with EINVAL (Rust's std asserts on that: it
// aborted backboardd, whose Metal runs on a Rust runtime).
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static int failures;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int ready;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

static void* signaller(void* arg)
{
	usleep(100000);
	pthread_mutex_lock(&mutex);
	ready = 1;
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&mutex);
	return NULL;
}

int main(void)
{
	struct timespec ms = {0, 1000000};
	int timedout = 0, other = 0;

	pthread_mutex_lock(&mutex);
	for (int i = 0; i < 200; i++)
	{
		int r = pthread_cond_timedwait_relative_np(&cond, &mutex, &ms);
		if (r == ETIMEDOUT)
			timedout++;
		else if (r != 0)
			other++;
	}
	check(timedout == 200, "200 timed waits with no signal time out");
	check(other == 0, "and none fails");

	pthread_t t;
	pthread_create(&t, NULL, signaller, NULL);
	struct timespec second = {1, 0};
	int r = 0;
	while (!ready && r == 0)
		r = pthread_cond_timedwait_relative_np(&cond, &mutex, &second);
	check(ready && r == 0, "a signal still wakes the waiter afterwards");
	pthread_mutex_unlock(&mutex);
	pthread_join(t, NULL);
	return failures;
}
