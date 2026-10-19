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
#include <mach/mach_error.h>
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

// write(), not printf(): stdout may be a file, fully buffered, which _exit() drops
static void hung(int sig)
{
	char line[128];
	int n;

	(void) sig;
	if (round_now < 0)
		n = snprintf(line, sizeof(line), "not ok - thread_suspend() of an exited thread returned within 60 s\n");
	else
		n = snprintf(line, sizeof(line),
			"not ok - thread_suspend() of a just-joined thread returned within 60 s (round %d of %d)\n",
			round_now, ROUNDS);
	write(STDOUT_FILENO, line, (n > 0 && n < (int) sizeof(line)) ? n : (int) sizeof(line) - 1);
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

	// What XNU answers depends on how far the joined thread's teardown has got (xnu-10002.1.13): running still,
	// KERN_SUCCESS (or KERN_TERMINATED); its port disabled but the thread not yet freed (ipc_thread_disable(),
	// osfmk/kern/ipc_tt.c:958), the port converts to THREAD_NULL, which thread_suspend() answers KERN_INVALID_ARGUMENT
	// (osfmk/kern/thread_act.c:400); reaped (thread_deallocate_complete() -> ipc_thread_terminate(),
	// osfmk/kern/thread.c:803, deallocates the port, ipc_tt.c:1047), our send right is a dead name and the call fails
	// with MACH_SEND_INVALID_DEST. Darling's release destroys the thread and its port together: only the last.
	kr = join_and_suspend();
	printf("# thread_suspend(): %d (%s)\n", kr, mach_error_string(kr));
	check(kr == KERN_SUCCESS || kr == KERN_TERMINATED || kr == KERN_INVALID_ARGUMENT || kr == MACH_SEND_INVALID_DEST,
		"thread_suspend() of an exited thread returns");

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
	// The "threads have gone" check below compares Linux thread counts: nothing else in this test may start a thread
	// between the two counts (a dispatch call would, and fail it falsely).
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
