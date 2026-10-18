// A fork child inherits the special ports its parent had when it forked. launchd relies on this to spawn a job:
// it sets its bootstrap port to the job's bootstrap, forks, and resets it to none straight away; the child then
// pings launchd through that port (_vproc_post_fork_ping) and exits 1 if it can't.
//
// darlingserver creates the child's task, copying the parent's ports, when the child first checks in, so fork()
// must not return in the parent before then. It used to wait for any checkin of a process, though, including a
// new thread's: every thread a child started let one later fork() of its parent return early, before its own
// child was registered, which then got no bootstrap port (about 1 launchd job in 10 to 20 exited 1, failing device
// boots).
//
// The test forks FORKS children the way launchd does, each sending a message through the bootstrap port it got:
// after a child that starts threads (what used to unbalance the parent's waits). A watchdog turns a hang into a
// failure. It uses only XNU's interface.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define FORKS 200
#define THREADS 4
#define STALL_SECONDS 30
// message IDs of the children's pings
#define PING_ID 0x666b0000

static atomic_int progress, failures;

static void* idle(void* arg)
{
	return arg;
}

// fails the test if no fork completes for STALL_SECONDS (a parent stuck in fork(), or a child that never ends)
static void* watchdog(void* arg)
{
	int last = -1, still = 0;
	(void) arg;

	for (;;)
	{
		sleep(1);
		int now = atomic_load(&progress);
		if (now != last)
		{
			last = now;
			still = 0;
		}
		else if (++still >= STALL_SECONDS)
		{
			printf("not ok - forks keep completing: none for %d s after %d\n", STALL_SECONDS, now);
			fflush(stdout);
			_exit(atomic_load(&failures) + 1);
		}
	}
	return NULL;
}

// a child that starts THREADS threads and exits
static int run_threaded_child(void)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0)
	{
		pthread_t threads[THREADS];
		for (int i = 0; i < THREADS; i++)
			if (pthread_create(&threads[i], NULL, idle, NULL) != 0)
				_exit(1);
		for (int i = 0; i < THREADS; i++)
			pthread_join(threads[i], NULL);
		_exit(0);
	}
	int status;
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

// in the child: send a message with ID `id` to the bootstrap port; the exit status says what failed
static void child_ping(int id)
{
	mach_port_t bootstrap = MACH_PORT_NULL;
	if (task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &bootstrap) != KERN_SUCCESS)
		_exit(2);
	if (!MACH_PORT_VALID(bootstrap))
		_exit(3); // no bootstrap port: what launchd's jobs got

	mach_msg_header_t msg = {
		.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0),
		.msgh_size = sizeof(msg),
		.msgh_remote_port = bootstrap,
		.msgh_id = id,
	};
	if (mach_msg(&msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(msg), 0, MACH_PORT_NULL, 5000, MACH_PORT_NULL)
		!= MACH_MSG_SUCCESS)
		_exit(4);
	_exit(0);
}

// forks FORKS children as launchd does; returns how many didn't reach `port`, or -1 if the test itself failed
static int fork_children(mach_port_t port, const char* what)
{
	mach_port_t self = mach_task_self();
	int lost = 0, first_fork = -1, first_status = -1;

	for (int i = 0; i < FORKS; i++)
	{
		if (task_set_special_port(self, TASK_BOOTSTRAP_PORT, port) != KERN_SUCCESS)
		{
			printf("not ok - %s: set the bootstrap port\n", what);
			return -1;
		}
		pid_t pid = fork();
		if (pid == 0)
			child_ping(PING_ID + i);
		// as launchd's runtime_fork() does: the parent resets its port as soon as fork() returns
		kern_return_t kr = task_set_special_port(self, TASK_BOOTSTRAP_PORT, MACH_PORT_NULL);
		if (pid < 0 || kr != KERN_SUCCESS)
		{
			printf("not ok - %s: fork %d: %s\n", what, i, pid < 0 ? strerror(errno) : "reset the bootstrap port");
			return -1;
		}

		int status;
		while (waitpid(pid, &status, 0) < 0)
		{
			if (errno != EINTR)
			{
				printf("not ok - %s: wait for child %d: %s\n", what, i, strerror(errno));
				return -1;
			}
		}
		int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

		struct {
			mach_msg_header_t header;
			mach_msg_trailer_t trailer;
		} ping;
		// the child sent its ping before it exited
		do
			kr = mach_msg(&ping.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(ping), port, 0, MACH_PORT_NULL);
		while (kr == MACH_RCV_INTERRUPTED);

		if (code != 0 || kr != MACH_MSG_SUCCESS || ping.header.msgh_id != PING_ID + i)
		{
			if (lost++ == 0)
			{
				first_fork = i;
				first_status = code;
			}
		}
		atomic_fetch_add(&progress, 1);
	}

	if (lost == 0)
		printf("ok - %s: %d children reached the bootstrap port they were forked with\n", what, FORKS);
	else
		// exit status 3: the child had no bootstrap port
		printf("not ok - %s: %d of %d children didn't reach the bootstrap port they were forked with (first: fork %d, "
			"exit status %d)\n", what, lost, FORKS, first_fork, first_status);
	return lost;
}

int main(void)
{
	mach_port_t self = mach_task_self(), port, saved;
	pthread_t watchdog_thread;

	if (mach_port_allocate(self, MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS
		|| mach_port_insert_right(self, port, port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS
		|| task_get_special_port(self, TASK_BOOTSTRAP_PORT, &saved) != KERN_SUCCESS
		|| pthread_create(&watchdog_thread, NULL, watchdog, NULL) != 0)
	{
		printf("not ok - set up\n");
		return 1;
	}

	if (run_threaded_child() != 0)
	{
		printf("not ok - a child starts %d threads\n", THREADS);
		atomic_fetch_add(&failures, 1);
	}
	else
		printf("ok - a child starts %d threads\n", THREADS);

	if (fork_children(port, "after a child's threads") != 0)
		atomic_fetch_add(&failures, 1);

	task_set_special_port(self, TASK_BOOTSTRAP_PORT, saved);
	mach_port_mod_refs(self, port, MACH_PORT_RIGHT_RECEIVE, -1);
	mach_port_deallocate(self, port);
	return atomic_load(&failures);
}
