// An mk_timer armed with mk_timer_arm_leeway() fires, as one armed with mk_timer_arm() does. Darling had no trap 95:
// the call failed, and Core Animation's render server, which times its next frame this way, ran no animation by
// itself (spinners stood still; an animation showed the frame of the last commit, until the app committed again).
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>

extern mach_port_name_t mk_timer_create(void);
extern kern_return_t mk_timer_destroy(mach_port_name_t name);
extern kern_return_t mk_timer_arm_leeway(mach_port_name_t name, uint64_t flags, uint64_t expire_time, uint64_t leeway);

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

int main(void)
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t delay = 200000000ull * tb.denom / tb.numer; // 200 ms

	mach_port_name_t timer = mk_timer_create();
	uint64_t start = mach_absolute_time();
	check(mk_timer_arm_leeway(timer, 1 /* MK_TIMER_CRITICAL */, start + delay, delay / 100) == KERN_SUCCESS,
		"mk_timer_arm_leeway arms the timer");

	struct { mach_msg_header_t header; char body[64]; } msg;
	kern_return_t kr = mach_msg(&msg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(msg), timer, 2000, MACH_PORT_NULL);
	uint64_t elapsed = mach_absolute_time() - start;
	check(kr == KERN_SUCCESS, "the timer fires");
	check(elapsed >= delay && elapsed < 10 * delay, "at its expiry time");

	mk_timer_destroy(timer);
	return failures;
}
