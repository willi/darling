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
// The test forks FORKS children the way launchd does, each sending a message through the bootstrap port it got,
// after a child that starts threads (what used to unbalance the parent's waits). Then children stop themselves before
// their checkin (Darling's libsystem_kernel has a switch for it, __darling_fork_stop_child), which keeps fork()
// waiting. Another thread either signals the forking thread and kills the child (an interrupted wait used to return
// early, and a dead child's fork() to wait for ever), or signals the child and lets it go on (its handler used to run
// on its parent's RPC socket, and crash darlingserver). A watchdog turns a hang into a failure. Apart from that switch
// it uses only XNU's interface.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FORKS 200
#define THREADS 4
#define STALL_SECONDS 30
// message IDs of the children's pings
#define PING_ID 0x666b0000
// children of each kind that stop before their checkin, and the signals the parent's fork() gets meanwhile
#define STOPS 4
#define SIGNALS 3

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

// receives child `i`'s ping on `port`, if it sent one before it exited
static int got_ping(mach_port_t port, int i)
{
	struct {
		mach_msg_header_t header;
		mach_msg_trailer_t trailer;
	} ping;
	kern_return_t kr;

	do
		kr = mach_msg(&ping.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(ping), port, 0, MACH_PORT_NULL);
	while (kr == MACH_RCV_INTERRUPTED);
	return kr == MACH_MSG_SUCCESS && ping.header.msgh_id == PING_ID + i;
}

static void sleep_ms(long ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000 };
	nanosleep(&t, NULL);
}

enum how { SIGNAL_PARENT, SIGNAL_CHILD };

// a signalled child's exit statuses when its handler didn't run, or its signal mask isn't its parent's
#define CHILD_NOT_HANDLED 5
#define CHILD_MASK_CHANGED 6

static pthread_t forker;
static atomic_int forks_started, forks_handled, signals_handled, child_stopped, parent_signals, kill_sent;
static _Atomic enum how how;

static void on_signal(int sig)
{
	(void) sig;
	atomic_fetch_add(&signals_handled, 1);
}

// For each fork, waits for the child to stop itself before its checkin (__darling_fork_stop_child). SIGNAL_PARENT:
// signals the forking thread SIGNALS times, sees whether fork() still waits, and kills the child. SIGNAL_CHILD:
// signals the child and lets it go on.
static void* stopper(void* arg)
{
	(void) arg;

	for (int handled = 0;; handled++)
	{
		while (atomic_load(&forks_started) == handled)
			sched_yield();

		// only a stop: a child that exited stays for its parent's waitpid()
		pid_t child = 0;
		for (int ms = 0; ms < 5000 && child == 0; ms++)
		{
			siginfo_t info;
			memset(&info, 0, sizeof(info));
			if (waitid(P_ALL, 0, &info, WSTOPPED | WNOHANG) == 0 && info.si_pid > 0 && info.si_code == CLD_STOPPED)
				child = info.si_pid;
			else
				sleep_ms(1);
		}

		if (child > 0)
		{
			atomic_store(&child_stopped, 1);
			if (atomic_load(&how) == SIGNAL_PARENT)
			{
				// each one handled by the forking thread, during its fork()
				int taken = 0;
				for (int i = 0; i < SIGNALS; i++)
				{
					int before = atomic_load(&signals_handled);
					if (pthread_kill(forker, SIGUSR1) != 0)
						break;
					for (int ms = 0; ms < 5000 && atomic_load(&signals_handled) == before; ms++)
						sleep_ms(1);
					if (atomic_load(&signals_handled) != before)
						taken++;
				}
				atomic_store(&parent_signals, taken);
				// a fork() that returns before this returned early
				atomic_store(&kill_sent, 1);
				kill(child, SIGKILL);
			}
			else
			{
				kill(child, SIGUSR1);
				kill(child, SIGCONT);
			}
		}
		atomic_store(&forks_handled, handled + 1);
	}
	return NULL;
}

// forks STOPS children of each kind that stop before their checkin; returns the failed checks
static int fork_stopped_children(mach_port_t port)
{
	mach_port_t self = mach_task_self();
	int not_stopped = 0, returned = 0, unsignalled = 0, not_killed = 0, lost = 0, lost_status = 0;
	int* stop_child = dlsym(RTLD_DEFAULT, "__darling_fork_stop_child");
	struct sigaction sa;
	pthread_t thread;

	if (stop_child == NULL)
	{
		printf("not ok - stopped children: libsystem_kernel has no __darling_fork_stop_child\n");
		return 1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	forker = pthread_self();
	if (sigaction(SIGUSR1, &sa, NULL) != 0 || pthread_create(&thread, NULL, stopper, NULL) != 0)
	{
		printf("not ok - stopped children: set up\n");
		return 1;
	}

	for (int i = 0; i < 2 * STOPS; i++)
	{
		enum how kind = (i % 2 == 0) ? SIGNAL_PARENT : SIGNAL_CHILD;
		atomic_store(&how, kind);
		atomic_store(&child_stopped, 0);
		atomic_store(&parent_signals, 0);
		atomic_store(&kill_sent, 0);
		if (task_set_special_port(self, TASK_BOOTSTRAP_PORT, port) != KERN_SUCCESS)
		{
			printf("not ok - stopped children: set the bootstrap port\n");
			return 1;
		}
		sigset_t mask;
		pthread_sigmask(SIG_SETMASK, NULL, &mask);
		int handled = atomic_load(&signals_handled);
		atomic_fetch_add(&forks_started, 1);
		*stop_child = 1;
		pid_t pid = fork();
		*stop_child = 0;
		if (pid == 0)
		{
			// The stopper's signal was pending when this fork() unblocked the child's signals: the handler has run,
			// and the mask is its parent's again.
			if (kind == SIGNAL_CHILD)
			{
				sigset_t now;
				pthread_sigmask(SIG_SETMASK, NULL, &now);
				if (atomic_load(&signals_handled) == handled)
					_exit(CHILD_NOT_HANDLED);
				if (memcmp(&now, &mask, sizeof(now)) != 0)
					_exit(CHILD_MASK_CHANGED);
			}
			child_ping(PING_ID + i);
		}
		int early = !atomic_load(&kill_sent);
		kern_return_t kr = task_set_special_port(self, TASK_BOOTSTRAP_PORT, MACH_PORT_NULL);
		if (pid < 0 || kr != KERN_SUCCESS)
		{
			printf("not ok - stopped children: fork %d: %s\n", i, pid < 0 ? strerror(errno) : "reset the port");
			return 1;
		}
		while (atomic_load(&forks_handled) != atomic_load(&forks_started))
			sleep_ms(1);

		int status;
		while (waitpid(pid, &status, 0) < 0)
		{
			if (errno != EINTR)
			{
				printf("not ok - stopped children: wait for child %d: %s\n", i, strerror(errno));
				return 1;
			}
		}
		int pinged = got_ping(port, i);
		if (!atomic_load(&child_stopped))
			not_stopped++;
		else if (kind == SIGNAL_PARENT)
		{
			// a signal must not end the wait: the parent would reset its port before the child got it
			if (early)
				returned++;
			if (atomic_load(&parent_signals) != SIGNALS)
				unsignalled++;
			if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
				not_killed++;
		}
		// it got the port it was forked with, and its signal didn't disturb its parent's fork()
		else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !pinged)
		{
			if (lost++ == 0)
				lost_status = status;
		}
		atomic_fetch_add(&progress, 1);
	}

	int failed = 0;
	if (not_stopped)
	{
		printf("not ok - children stop before their checkin (__darling_fork_stop_child): %d of %d didn't\n",
			not_stopped, 2 * STOPS);
		failed++;
	}
	// else a fork() that returns at a signal could pass
	if (unsignalled == 0)
		printf("ok - the forking thread takes %d signals while its fork() waits\n", SIGNALS);
	else
	{
		printf("not ok - the forking thread takes %d signals while its fork() waits: it didn't, for %d of %d\n",
			SIGNALS, unsignalled, STOPS);
		failed++;
	}
	if (returned == 0)
		printf("ok - fork() waits for a child that hasn't checked in while signalled %d times\n", SIGNALS);
	else
	{
		printf("not ok - fork() waits for a child that hasn't checked in while signalled %d times: it returned for %d "
			"of %d\n", SIGNALS, returned, STOPS);
		failed++;
	}
	// a fork() that waits for ever stops the forks, and the watchdog fails the test
	if (not_killed == 0)
		printf("ok - fork() returns once a child that hasn't checked in is killed\n");
	else
	{
		printf("not ok - fork() of a child killed before its checkin: %d exited otherwise\n", not_killed);
		failed++;
	}
	if (lost == 0)
		printf("ok - a child signalled before its checkin reaches the bootstrap port it was forked with\n");
	else
	{
		printf("not ok - a child signalled before its checkin: %d of %d didn't take the signal and reach the bootstrap "
			"port it was forked with (first: wait status 0x%x)\n", lost, STOPS, lost_status);
		failed++;
	}
	return failed;
}

int main(void)
{
	mach_port_t self = mach_task_self(), port, saved;
	pthread_t watchdog_thread;

	// a line at a time, so a crash leaves the checks before it
	setvbuf(stdout, NULL, _IOLBF, 0);

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

	atomic_fetch_add(&failures, fork_stopped_children(port));

	task_set_special_port(self, TASK_BOOTSTRAP_PORT, saved);
	mach_port_mod_refs(self, port, MACH_PORT_RIGHT_RECEIVE, -1);
	mach_port_deallocate(self, port);
	return atomic_load(&failures);
}
