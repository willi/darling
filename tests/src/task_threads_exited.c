// task_threads() lists a task's live threads only: one that has exited leaves the list (XNU's thread_terminate_daemon()
// takes it off its task's threads once it has stopped running), the main thread too, when it exits as dispatch_main()
// makes it; and an exited thread is gone, its port answering no more calls. darlingserver kept every thread that had
// checked out, with its port and in its task's list, for ever: a checkout's reply bypassed the thread, so its call stayed
// active, and a thread with an active call is never released. dyld's process scavenger, and CoreSymbolication, suspend
// each thread task_threads() lists.
// It only relies on XNU's interface, so it runs on macOS too (the main-thread check takes root there).
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define THREADS 50

static int failures;
static uint64_t exited_ids[THREADS];
static mach_port_t exited_ports[THREADS];

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	fflush(stdout);
	if (!ok)
		failures++;
}

static void* worker(void* arg)
{
	int i = (int) (intptr_t) arg;

	pthread_threadid_np(NULL, &exited_ids[i]);
	exited_ports[i] = mach_thread_self();
	return NULL;
}

static void* sleeper(void* arg)
{
	for (;;)
		pause();
	return arg;
}

// the IDs of the task's threads, as task_threads() lists them; their count, or -1
static int thread_ids(task_t task, uint64_t* ids, int max)
{
	thread_act_array_t threads;
	mach_msg_type_number_t count;

	if (task_threads(task, &threads, &count) != KERN_SUCCESS)
		return -1;
	for (mach_msg_type_number_t i = 0; i < count; i++)
	{
		thread_identifier_info_data_t info;
		mach_msg_type_number_t info_count = THREAD_IDENTIFIER_INFO_COUNT;

		if (i < (mach_msg_type_number_t) max)
			ids[i] = (thread_info(threads[i], THREAD_IDENTIFIER_INFO, (thread_info_t) &info, &info_count) == KERN_SUCCESS)
				? info.thread_id : 0;
		mach_port_deallocate(mach_task_self(), threads[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t) threads, count * sizeof(*threads));
	return (int) count;
}

// how many of these thread ports still answer thread_info()
static int answering(const mach_port_t* ports, int n)
{
	int count = 0;

	for (int i = 0; i < n; i++)
	{
		thread_basic_info_data_t info;
		mach_msg_type_number_t info_count = THREAD_BASIC_INFO_COUNT;

		count += thread_info(ports[i], THREAD_BASIC_INFO, (thread_info_t) &info, &info_count) == KERN_SUCCESS;
	}
	return count;
}

// how many of the task's threads are among `ids`
static int listed(task_t task, const uint64_t* ids, int n)
{
	uint64_t now[THREADS + 16];
	int count = thread_ids(task, now, THREADS + 16), found = 0;

	for (int i = 0; i < count && i < THREADS + 16; i++)
		for (int j = 0; j < n; j++)
			found += now[i] == ids[j];
	return found;
}

int main(void)
{
	int created = 0, still, pipefd[2], count = -1;
	pid_t child;
	task_t task;
	char byte;

	for (int i = 0; i < THREADS; i++)
	{
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker, (void*) (intptr_t) created) == 0)
		{
			pthread_join(thread, NULL);
			created++;
		}
	}

	// a thread is off the list soon after it has exited, not at once
	still = listed(mach_task_self(), exited_ids, created);
	for (int i = 0; i < 200 && still > 0; i++)
	{
		usleep(10000);
		still = listed(mach_task_self(), exited_ids, created);
	}
	printf("%s - task_threads() lists none of %d threads that have exited (%d still listed)\n",
		created == THREADS && still == 0 ? "ok" : "not ok", created, still);
	fflush(stdout);
	failures += !(created == THREADS && still == 0);

	// and is gone: its port answers no more calls, once XNU has disabled it (ipc_thread_disable(), as the thread terminates).
	// darlingserver kept the thread, and its port, for ever.
	still = answering(exited_ports, created);
	for (int i = 0; i < 200 && still > 0; i++)
	{
		usleep(10000);
		still = answering(exited_ports, created);
	}
	printf("%s - none of the %d threads that have exited answers thread_info() (%d still do)\n",
		created == THREADS && still == 0 ? "ok" : "not ok", created, still);
	fflush(stdout);
	failures += !(created == THREADS && still == 0);
	for (int i = 0; i < created; i++)
		mach_port_deallocate(mach_task_self(), exited_ports[i]);

	// a process whose main thread has exited, with another thread left
	pipe(pipefd);
	child = fork();
	if (child == 0)
	{
		pthread_t thread;
		pthread_create(&thread, NULL, sleeper, NULL);
		write(pipefd[1], "", 1);
		pthread_exit(NULL);
	}
	// (the child's write end only: a child that dies before writing ends the read)
	close(pipefd[1]);
	read(pipefd[0], &byte, 1);
	if (task_for_pid(mach_task_self(), child, &task) != KERN_SUCCESS)
	{
		printf("ok - # SKIP task_for_pid() of a child (root or an entitlement on macOS)\n");
	}
	else
	{
		uint64_t ids[4];
		for (int i = 0; i < 1000 && (count = thread_ids(task, ids, 4)) != 1; i++)
			usleep(10000);
		check(count == 1, "a process whose main thread has exited lists its other thread only");
	}
	if (child > 0) // a failed fork() would make this kill(-1): every process the test may signal
		kill(child, SIGKILL);

	return failures;
}
