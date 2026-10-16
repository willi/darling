// thread_suspend() on a thread that has exited answers (KERN_TERMINATED, say) rather than waiting for ever.
// darlingserver signalled the thread to stop and waited for it to say it had; a thread that had already exited, while
// darlingserver still counted it as alive, never did. XCTest symbolicating a failure (CoreSymbolication's
// _dyld_process_info_create() suspends every other thread of its own task) hung that way.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

static void* worker(void* arg)
{
	return arg;
}

static void hung(int sig)
{
	(void) sig;
	printf("not ok - thread_suspend() of an exited thread returned within 10 s\n");
	_exit(1);
}

int main(void)
{
	thread_act_array_t threads;
	mach_msg_type_number_t count;
	pthread_t thread;
	mach_port_t port;
	kern_return_t kr;

	signal(SIGALRM, hung);
	alarm(10);

	pthread_create(&thread, NULL, worker, NULL);
	// a send right of our own, which outlives the thread, as task_threads()'s are
	port = pthread_mach_thread_np(thread);
	mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1);
	pthread_join(thread, NULL);

	kr = thread_suspend(port);
	check(kr != KERN_SUCCESS, "thread_suspend() of an exited thread fails");
	if (kr == KERN_SUCCESS)
		thread_resume(port);

	// and suspending the task's other threads, as dyld's process scavenger does, comes back
	kr = task_threads(mach_task_self(), &threads, &count);
	check(kr == KERN_SUCCESS, "task_threads()");
	for (mach_msg_type_number_t i = 0; kr == KERN_SUCCESS && i < count; i++)
	{
		if (threads[i] != mach_thread_self() && thread_suspend(threads[i]) == KERN_SUCCESS)
			thread_resume(threads[i]);
	}
	check(1, "suspending and resuming the task's other threads comes back");

	alarm(0);
	return failures;
}
