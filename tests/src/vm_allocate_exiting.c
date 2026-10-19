// mach_vm_allocate() into another task returns while that task's threads come and go, and after its main thread has
// exited (as dispatch_main() leaves it). darlingserver maps memory in another process through one of its threads: an
// S2C call, which the thread runs when darlingserver signals it. A thread that has just started often has a call that
// darlingserver has received but not started, and the signal handler's interrupt_enter then waits behind that call.
// darlingserver scheduled such a thread twice, and the spare run resumed the thread's S2C wait with no wakeup:
// darlingserver panicked ("semaphore_block"), and every process lost its server.
// On macOS, task_for_pid() needs root or an entitlement; the test says so rather than failing.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#define ROUNDS 200

static volatile int round_now;
static volatile pid_t child;

// write(), not printf(): stdout may be a file, fully buffered, which _exit() drops
static void hung(int sig)
{
	char line[128];
	int n = snprintf(line, sizeof(line), "not ok - mach_vm_allocate() into a task whose threads come and go returned "
		"within 60 s (round %d of %d)\n", round_now, ROUNDS);

	(void) sig;
	write(STDOUT_FILENO, line, (n > 0 && n < (int) sizeof(line)) ? n : (int) sizeof(line) - 1);
	kill(child, SIGKILL);
	_exit(1);
}

// each thread starts the next one and exits: the child's oldest thread is always about to exit
static void* chain_link(void* arg)
{
	pthread_t next;
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	while (pthread_create(&next, &attr, chain_link, NULL) != 0)
		usleep(1000);
	return arg;
}

int main(void)
{
	mach_port_t task;
	kern_return_t kr = KERN_SUCCESS;
	int pipefd[2];
	char byte;

	pipe(pipefd);
	child = fork();
	if (child == 0)
	{
		pthread_t first;

		pthread_create(&first, NULL, chain_link, NULL);
		write(pipefd[1], "", 1);
		// the main thread exits, the process goes on
		pthread_exit(NULL);
	}
	if (child < 0)
	{
		// nothing to allocate into, and a kill(-1) below would signal every process the test may
		printf("not ok - fork()\n");
		return 1;
	}
	read(pipefd[0], &byte, 1);

	if (task_for_pid(mach_task_self(), child, &task) != KERN_SUCCESS)
	{
		printf("ok - # SKIP task_for_pid() of a child (root or an entitlement on macOS)\n");
		kill(child, SIGKILL);
		return 0;
	}

	signal(SIGALRM, hung);
	alarm(60);
	for (round_now = 0; round_now < ROUNDS && kr == KERN_SUCCESS; round_now++)
	{
		mach_vm_address_t address = 0;

		kr = mach_vm_allocate(task, &address, vm_page_size, VM_FLAGS_ANYWHERE);
		if (kr == KERN_SUCCESS)
			kr = mach_vm_deallocate(task, address, vm_page_size);
	}
	alarm(0);

	printf("%s - mach_vm_allocate() into a task whose threads come and go returns, %d times (%d: %s)\n",
		kr == KERN_SUCCESS ? "ok" : "not ok", ROUNDS, kr, mach_error_string(kr));
	kill(child, SIGKILL);
	return kr != KERN_SUCCESS;
}
