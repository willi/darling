// mach_vm_allocate() into another task returns while that task's threads come and go, and after its main thread has
// exited (as dispatch_main() leaves it). darlingserver maps memory in another process through one of its threads: an
// S2C call, which the thread runs when darlingserver signals it. This test caught three regressions there:
// - darlingserver scheduled a thread twice when its signal handler's interrupt_enter waited behind a call it had
//   received but not started, and the spare run resumed the thread's S2C wait with no wakeup: darlingserver panicked
//   ("semaphore_block"), and every process lost its server (darlingserver's single-schedule fix);
// - the S2C call picked a thread that was exiting, and failed (KERN_FAILURE or KERN_RESOURCE_SHORTAGE) rather than
//   going to another thread (darlingserver's s2c-dead-thread);
// - the RPC hooks' signal mask restore unblocked darlingserver's signals inside their own handler, so S2C signals
//   nested until the child's signal stack overflowed: the child was killed by SIGSEGV (darling-xnu's rpc-sigmask).
// On macOS, task_for_pid() needs root or an entitlement; the test says so rather than failing.
//
// Prints one "ok"/"not ok" line per check, and a "# child:" line with the child's fate; the exit status is the number
// of failed checks.

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROUNDS 200

static volatile int round_now;
static volatile pid_t child;

// write(), not printf(): stdout may be a file, fully buffered, which _exit() drops
static void say(const char* line, int n, int size)
{
	write(STDOUT_FILENO, line, (n > 0 && n < size) ? n : size - 1);
}

// whether the child is still alive, or how it ended, before the test kills it
static void child_fate(void)
{
	char line[64];
	int status, n;

	if (waitpid(child, &status, WNOHANG) != child)
		n = snprintf(line, sizeof(line), "# child: alive\n");
	else if (WIFSIGNALED(status))
		n = snprintf(line, sizeof(line), "# child: killed by signal %d\n", WTERMSIG(status));
	else
		n = snprintf(line, sizeof(line), "# child: exited %d\n", WEXITSTATUS(status));
	say(line, n, sizeof(line));
}

static void hung(int sig)
{
	char line[128];
	int n = snprintf(line, sizeof(line), "not ok - mach_vm_allocate() into a task whose threads come and go returned "
		"within 60 s (round %d of %d)\n", round_now, ROUNDS);

	(void) sig;
	say(line, n, sizeof(line));
	child_fate();
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
	// a child that dies before its write ends the read (no writer left), and the alarm bounds the rest
	close(pipefd[1]);
	signal(SIGALRM, hung);
	alarm(60);
	if (read(pipefd[0], &byte, 1) != 1)
	{
		printf("not ok - child died before starting\n");
		fflush(stdout);
		child_fate();
		return 1;
	}

	if (task_for_pid(mach_task_self(), child, &task) != KERN_SUCCESS)
	{
		printf("ok - # SKIP task_for_pid() of a child (root or an entitlement on macOS)\n");
		kill(child, SIGKILL);
		return 0;
	}

	for (round_now = 0; round_now < ROUNDS && kr == KERN_SUCCESS; round_now++)
	{
		mach_vm_address_t address = 0;

		kr = mach_vm_allocate(task, &address, vm_page_size, VM_FLAGS_ANYWHERE);
		if (kr == KERN_SUCCESS)
			kr = mach_vm_deallocate(task, address, vm_page_size);
	}
	alarm(0);

	// the loop's increment runs before its check ends it: a failure was in round round_now - 1
	if (kr == KERN_SUCCESS)
		printf("ok - mach_vm_allocate() into a task whose threads come and go returns, %d times\n", ROUNDS);
	else
		printf("not ok - mach_vm_allocate() into a task whose threads come and go returns, %d times "
			"(round %d: %d, %s)\n", ROUNDS, round_now - 1, kr, mach_error_string(kr));
	fflush(stdout);
	child_fate();
	kill(child, SIGKILL);
	return kr != KERN_SUCCESS;
}
