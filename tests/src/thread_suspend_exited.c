// thread_suspend() of a thread that has exited, or is exiting, returns, and the thread goes.
// darlingserver signalled the thread to stop and waited for it to say it had; a thread that had already exited, while
// darlingserver still counted it as alive, never did. XCTest symbolicating a failure (CoreSymbolication's
// _dyld_process_info_create() suspends every other thread of its own task) hung that way.
//
// pthread_join() returns once the thread has marked itself exited (_pthread_terminate()), before its
// __bsdthread_terminate(), so on XNU too a thread_suspend() right after it can find the thread still running, and
// succeed (thread_wait() then waits until it's off CPU); only that it returns is checked.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <dirent.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

// create, join and suspend, each time
#define ROUNDS 300

static int failures;
static volatile int round_now = -1;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	fflush(stdout);
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
	if (round_now < 0)
		printf("not ok - thread_suspend() of an exited thread returned within 60 s\n");
	else
		printf("not ok - thread_suspend() of a just-joined thread returned within 60 s (round %d of %d)\n",
			round_now, ROUNDS);
	_exit(1);
}

// the process's threads, from Linux's /proc (they go when they exit); -1 where there's none (macOS)
static int linux_threads(void)
{
	DIR* dir = opendir("/proc/self/task");
	struct dirent* entry;
	int count = 0;

	if (!dir)
		return -1;
	while ((entry = readdir(dir)))
		count += entry->d_name[0] != '.';
	closedir(dir);
	return count;
}

// suspends (and resumes) a thread just joined; the send right is the caller's own, which outlives the thread, as
// task_threads()'s are
static kern_return_t join_and_suspend(void)
{
	pthread_t thread;
	mach_port_t port;
	kern_return_t kr;

	pthread_create(&thread, NULL, worker, NULL);
	port = pthread_mach_thread_np(thread);
	mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1);
	pthread_join(thread, NULL);

	kr = thread_suspend(port);
	if (kr == KERN_SUCCESS)
		thread_resume(port);
	mach_port_deallocate(mach_task_self(), port);
	return kr;
}

int main(void)
{
	thread_act_array_t threads;
	mach_msg_type_number_t count;
	kern_return_t kr;
	int before, after;

	signal(SIGALRM, hung);
	alarm(60);

	kr = join_and_suspend();
	check(kr == KERN_SUCCESS || kr == KERN_TERMINATED, "thread_suspend() of an exited thread returns");

	// and suspending the task's other threads, as dyld's process scavenger does, comes back
	kr = task_threads(mach_task_self(), &threads, &count);
	check(kr == KERN_SUCCESS, "task_threads()");
	for (mach_msg_type_number_t i = 0; kr == KERN_SUCCESS && i < count; i++)
	{
		if (threads[i] != mach_thread_self() && thread_suspend(threads[i]) == KERN_SUCCESS)
			thread_resume(threads[i]);
	}
	check(1, "suspending and resuming the task's other threads comes back");

	// Regression: darlingserver's suspend signal could come while the thread was checking out of darlingserver.
	// Its handler's interrupt_enter got the checkout's reply, and pushed it back to a darlingserver that had let
	// the thread go: the thread stayed, blocked for ever, and thread_suspend() waited for ever for it to stop.
	before = linux_threads();
	for (round_now = 0; round_now < ROUNDS; round_now++)
		join_and_suspend();
	round_now = -1;
	check(1, "thread_suspend() of a just-joined thread returns, every time");

	after = linux_threads();
	// the last one's Linux thread may still be on its way out
	for (int i = 0; i < 200 && after > before; i++)
	{
		usleep(10000);
		after = linux_threads();
	}
	if (before < 0)
		printf("ok - # SKIP no /proc/self/task: the joined threads have gone\n");
	else
	{
		char what[96];
		snprintf(what, sizeof(what), "the joined threads have gone (%d threads before, %d after)", before, after);
		check(after <= before, what);
	}

	alarm(0);
	return failures;
}
