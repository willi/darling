// getpriority() as XNU's nice value (B62). Darling's getpriority() returned Linux's syscall value, 20 - nice: after
// setpriority(5) it read 15, and nice(1), which sets getpriority() + 1, then put the process at nice 16 (read as 4).
//
// - After setpriority(19): 19, and nice(1) there stays at 19 (libc's nice() clamps at NZERO - 1). This needs no
//   privilege, so it runs whoever runs the test; the old code read 1, and its nice(1) asked for 2.
// - After setpriority(5): 5, and nice(1) then gives 6.
// - A negative nice value reads as itself: -5 after setpriority(-5).
// The last two raise the priority, so they're skipped (an "ok - # skip" line) where the process may not (EACCES or
// EPERM). It uses only XNU's interface.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>

static int failed;

static void check(int ok, const char* what, long got)
{
	printf("%s - %s (got %ld)\n", ok ? "ok" : "not ok", what, got);
	if (!ok)
		failed++;
}

// setpriority() of this process; 0 when it may not raise its priority (printing a skip line), else 1
static int set(int value)
{
	if (setpriority(PRIO_PROCESS, 0, value) == 0)
		return 1;
	if (errno == EACCES || errno == EPERM)
	{
		printf("ok - # skip: may not set nice %d\n", value);
		return 0;
	}
	check(0, "setpriority succeeds", errno);
	return 0;
}

// getpriority() of this process; errno 0 unless it failed (-1 is a valid answer)
static int get(void)
{
	errno = 0;
	return getpriority(PRIO_PROCESS, 0);
}

int main(void)
{
	int r;

	if (set(19))
	{
		r = get();
		check(r == 19 && errno == 0, "getpriority after setpriority(19): 19", r);
		errno = 0;
		r = nice(1);
		check(r == 19 && errno == 0, "nice(1) at 19: 19", r);
	}
	if (set(5))
	{
		r = get();
		check(r == 5 && errno == 0, "getpriority after setpriority(5): 5", r);
		errno = 0;
		r = nice(1);
		check(r == 6 && errno == 0, "nice(1) after that: 6", r);
	}
	if (set(-5))
	{
		r = get();
		check(r == -5 && errno == 0, "getpriority after setpriority(-5): -5", r);
	}
	return failed;
}
