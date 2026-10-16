// EVFILT_TIMER re-armed as libdispatch arms its timers: one knote, given each new deadline with EV_ADD | EV_ENABLE, in
// absolute Mach time. The knote must take the new deadline, earlier or later (launchd_sim SIGKILLs a daemon that
// ignored its SIGTERM on a 10-second timer, which used to wait for an earlier 30-second deadline instead). It only
// relies on XNU's interface, so it runs on macOS too.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <sys/event.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define TIMER_IDENT 1

static int failures;
static mach_timebase_info_data_t timebase;

static void check(bool ok, const char* name, const char* detail)
{
	if (ok)
		printf("ok - %s\n", name);
	else
	{
		printf("not ok - %s: %s\n", name, detail);
		failures++;
	}
	fflush(stdout);
}

static uint64_t ms_to_mach(uint64_t ms)
{
	return ms * 1000000 * timebase.denom / timebase.numer;
}

// arms the timer for `ms` milliseconds from now, as libdispatch does
static bool arm(int kq, uint64_t ms)
{
	struct kevent64_s kev;

	EV_SET64(&kev, TIMER_IDENT, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT,
		NOTE_ABSOLUTE | NOTE_MACHTIME | NOTE_LEEWAY, mach_absolute_time() + ms_to_mach(ms), 0, 0, 0);
	return kevent64(kq, &kev, 1, NULL, 0, 0, NULL) == 0;
}

// milliseconds until the timer fires, or -1 if it doesn't within `timeout_ms`
static long wait_timer(int kq, long timeout_ms)
{
	struct kevent64_s kev;
	struct timespec timeout = { timeout_ms / 1000, (timeout_ms % 1000) * 1000000 };
	uint64_t start = mach_absolute_time();
	int n = kevent64(kq, NULL, 0, &kev, 1, 0, &timeout);

	if (n != 1 || kev.filter != EVFILT_TIMER || kev.ident != TIMER_IDENT)
		return -1;
	return (long) ((mach_absolute_time() - start) * timebase.numer / timebase.denom / 1000000);
}

int main(void)
{
	char detail[64];
	long ms;
	int kq;

	mach_timebase_info(&timebase);
	kq = kqueue();

	// an earlier deadline: the timer fires then, not at the first one
	check(arm(kq, 10000) && arm(kq, 200), "re-arm earlier", "kevent failed");
	ms = wait_timer(kq, 3000);
	snprintf(detail, sizeof(detail), "fired after %ld ms", ms);
	check(ms >= 0 && ms < 3000, "the earlier deadline replaces the first", detail);

	// a later deadline: the timer doesn't fire at the first one
	check(arm(kq, 200) && arm(kq, 1500), "re-arm later", "kevent failed");
	ms = wait_timer(kq, 1000);
	snprintf(detail, sizeof(detail), "fired after %ld ms", ms);
	check(ms == -1, "the later deadline replaces the first", detail);
	ms = wait_timer(kq, 3000);
	snprintf(detail, sizeof(detail), "fired after another %ld ms", ms);
	check(ms >= 0, "the timer fires at the later deadline", detail);

	close(kq);
	return failures;
}
