// thread_info() and thread_policy_get() answer every flavor as XNU does. darlingserver used to abort on any
// thread_info() flavor but THREAD_BASIC_INFO and THREAD_IDENTIFIER_INFO, and on any thread_policy_get(): SpringBoard
// asked for another while starting up, and every process in the prefix died with the server.
//
// It only relies on XNU's interface, so it runs on macOS too. Prints one "ok"/"not ok" line per check; the exit status
// is the number of failed checks.

#include <mach/mach.h>
#include <stdio.h>

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

int main(void)
{
	thread_act_t self = mach_thread_self();

	thread_extended_info_data_t ext;
	mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
	check(thread_info(self, THREAD_EXTENDED_INFO, (thread_info_t) &ext, &count) == KERN_SUCCESS &&
			count == THREAD_EXTENDED_INFO_COUNT && ext.pth_run_state == TH_STATE_RUNNING,
		"THREAD_EXTENDED_INFO: the calling thread is running");

	// one answer to how the thread is scheduled: basic and extended info agree with thread_policy_get()
	thread_basic_info_data_t basic;
	count = THREAD_BASIC_INFO_COUNT;
	check(thread_info(self, THREAD_BASIC_INFO, (thread_info_t) &basic, &count) == KERN_SUCCESS &&
			basic.policy == POLICY_TIMESHARE && ext.pth_policy == POLICY_TIMESHARE,
		"THREAD_BASIC_INFO and THREAD_EXTENDED_INFO: a time-shared thread");

	policy_timeshare_info_data_t ts;
	count = POLICY_TIMESHARE_INFO_COUNT;
	check(thread_info(self, THREAD_SCHED_TIMESHARE_INFO, (thread_info_t) &ts, &count) == KERN_SUCCESS &&
			ts.base_priority > 0 && ts.base_priority <= ts.max_priority,
		"THREAD_SCHED_TIMESHARE_INFO: a time-shared thread's priorities");

	integer_t buf[64];
	count = 64;
	check(thread_info(self, 0x7fff, buf, &count) == KERN_INVALID_ARGUMENT, "an unknown flavor is an invalid argument");

	thread_latency_qos_policy_data_t qos;
	boolean_t get_default = FALSE;
	count = THREAD_LATENCY_QOS_POLICY_COUNT;
	check(thread_policy_get(self, THREAD_LATENCY_QOS_POLICY, (thread_policy_t) &qos, &count, &get_default) ==
				KERN_SUCCESS &&
			count == THREAD_LATENCY_QOS_POLICY_COUNT,
		"thread_policy_get(THREAD_LATENCY_QOS_POLICY)");

	thread_extended_policy_data_t policy;
	get_default = FALSE;
	count = THREAD_EXTENDED_POLICY_COUNT;
	check(thread_policy_get(self, THREAD_EXTENDED_POLICY, (thread_policy_t) &policy, &count, &get_default) ==
				KERN_SUCCESS &&
			policy.timeshare,
		"thread_policy_get(THREAD_EXTENDED_POLICY): time-shared");

	mach_port_deallocate(mach_task_self(), self);
	return failures;
}
