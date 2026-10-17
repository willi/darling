// clock_gettime(CLOCK_MONOTONIC) never goes back. It's the time of day less the boot time (kern.boottime), which
// Darling took as the time less the uptime, both in whole seconds changing at different moments: the boot time moved
// by a second from call to call, and the clock jumped a second back and forth.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <stdio.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

int main(void)
{
	struct timespec prev = { 0, 0 }, now;
	int back = 0, boottimes = 0;
	struct timeval first = { 0, 0 };
	time_t start = time(NULL);

	// across at least two changes of the second
	while (time(NULL) < start + 3)
	{
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec < prev.tv_sec || (now.tv_sec == prev.tv_sec && now.tv_nsec < prev.tv_nsec))
			back++;
		prev = now;

		struct timeval bt;
		size_t len = sizeof(bt);
		int mib[2] = { CTL_KERN, KERN_BOOTTIME };
		sysctl(mib, 2, &bt, &len, NULL, 0);
		if (!first.tv_sec)
			first = bt;
		else if (bt.tv_sec != first.tv_sec || bt.tv_usec != first.tv_usec)
			boottimes++;
	}

	check(boottimes == 0, "kern.boottime is the same every time");
	check(back == 0, "CLOCK_MONOTONIC never goes back");
	return failures;
}
