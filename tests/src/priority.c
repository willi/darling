// getpriority() and setpriority() as XNU's (B62). Darling's getpriority() returned Linux's syscall value, 20 - nice:
// a process at nice 0 read 20, so nice(1), which sets getpriority() + 1, put it at Linux's lowest priority, 19. And
// both returned an error as a negative number (-3 for ESRCH) instead of -1 with errno set.
//
// - A pid that doesn't exist: -1 with ESRCH, from both.
// - This process starts at nice 0 and reads 0; after setpriority(5), 5; after nice(1), 6 (nice returns it too).
// - A negative nice value reads as itself: -5 after setpriority(-5). Skipped where this process may not raise its
//   priority (EACCES or EPERM).
// It uses only XNU's interface.
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

// getpriority() of this process; errno 0 unless it failed (-1 is a valid answer)
static int get(void)
{
	errno = 0;
	return getpriority(PRIO_PROCESS, 0);
}

int main(void)
{
	const id_t nopid = 999999;
	int r;

	errno = 0;
	r = getpriority(PRIO_PROCESS, nopid);
	check(r == -1 && errno == ESRCH, "getpriority of a pid that doesn't exist: -1, ESRCH", r);
	errno = 0;
	r = setpriority(PRIO_PROCESS, nopid, 1);
	check(r == -1 && errno == ESRCH, "setpriority of a pid that doesn't exist: -1, ESRCH", r);

	r = get();
	check(r == 0 && errno == 0, "getpriority at the start: 0", r);
	r = setpriority(PRIO_PROCESS, 0, 5);
	check(r == 0, "setpriority(5) succeeds", r);
	r = get();
	check(r == 5 && errno == 0, "getpriority after setpriority(5): 5", r);
	errno = 0;
	r = nice(1);
	check(r == 6 && errno == 0, "nice(1) after that: 6", r);
	r = get();
	check(r == 6 && errno == 0, "getpriority after nice(1): 6", r);

	if (setpriority(PRIO_PROCESS, 0, -5) != 0 && (errno == EACCES || errno == EPERM))
		printf("ok - # skip: may not raise its priority\n");
	else
	{
		r = get();
		check(r == -5 && errno == 0, "getpriority after setpriority(-5): -5", r);
	}
	return failed;
}
