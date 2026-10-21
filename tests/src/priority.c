// getpriority() as XNU's nice value (B62). Darling's getpriority() returned Linux's syscall value, 20 - nice: after
// setpriority(5) it read 15, and nice(1), which sets getpriority() + 1, then put the process at nice 16 (read as 4).
//
// - After setpriority(5): 5. After nice(1): 6, from nice() and from getpriority().
// - A negative nice value reads as itself: -5 after setpriority(-5).
// It sets each value first, so it holds whatever nice value it starts at (`nice -n 19` runs gave 19). Skipped where this
// process may not raise its priority (EACCES or EPERM). It uses only XNU's interface.
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
static int set(int nice)
{
	if (setpriority(PRIO_PROCESS, 0, nice) == 0)
		return 1;
	if (errno == EACCES || errno == EPERM)
	{
		printf("ok - # skip: may not set nice %d\n", nice);
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

	if (set(5))
	{
		r = get();
		check(r == 5 && errno == 0, "getpriority after setpriority(5): 5", r);
		errno = 0;
		r = nice(1);
		check(r == 6 && errno == 0, "nice(1) after that: 6", r);
		r = get();
		check(r == 6 && errno == 0, "getpriority after nice(1): 6", r);
	}
	if (set(-5))
	{
		r = get();
		check(r == -5 && errno == 0, "getpriority after setpriority(-5): -5", r);
	}
	return failed;
}
