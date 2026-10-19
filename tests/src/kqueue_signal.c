// EVFILT_SIGNAL as XNU has it, which Apple's libdispatch builds its signal sources on (launchd_sim stops a device
// when one sees launchd's SIGTERM): the filter records every signal delivered to the process, whichever thread takes
// it and whatever its disposition. Everything runs after the main thread has exited, as dispatch_main() leaves it.
// It only relies on XNU's interface, so it runs on macOS too.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <sys/event.h>
#include <sys/wait.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SIG SIGUSR1

static int failures;
static volatile sig_atomic_t handled;

static void check(bool ok, const char* name, const char* detail)
{
	if (ok)
		printf("ok - %s\n", name);
	else
	{
		printf("not ok - %s: %s\n", name, detail);
		failures++;
	}
	fflush(stdout);
}

static void handler(int sig)
{
	(void) sig;
	handled++;
}

static bool change(int kq, uint16_t flags)
{
	struct kevent kev;

	EV_SET(&kev, SIG, EVFILT_SIGNAL, flags, 0, 0, NULL);
	return kevent(kq, &kev, 1, NULL, 0, NULL) == 0;
}

// the event's data (the times the signal arrived), or -1 if none came within the timeout
static long wait_signal(int kq, long ms)
{
	struct kevent kev;
	struct timespec timeout = { ms / 1000, (ms % 1000) * 1000000 };
	int n = kevent(kq, NULL, 0, &kev, 1, &timeout);

	if (n == 1 && kev.filter == EVFILT_SIGNAL && kev.ident == SIG)
		return (long) kev.data;
	return -1;
}

// Waits until the handler has run `count` times in all. A knote records a signal before its handler runs (XNU's
// when it's sent, Darling's on the thread that takes it), so whatever a signal records is there by then, and one sent
// next can't merge with it. Delivery may take over a second on a loaded host: a check that gave it a fixed time saw
// it late, or a later check did.
static bool wait_handled(int count)
{
	for (int i = 0; i < 1000 && handled < count; i++)
		usleep(10000);
	return handled >= count;
}

// takes the process's signals while the checking thread blocks them
static void* receiver(void* arg)
{
	(void) arg;
	for (;;)
		pause();
	return NULL;
}

static void* run(void* arg)
{
	pthread_t thread;
	sigset_t set;
	long data;
	char detail[64];
	pid_t pid;
	int kq = kqueue(), kq2 = kqueue();
	bool ok;

	(void) arg;
	sigemptyset(&set);
	sigaddset(&set, SIG);
	// main() blocked it until it exited; now only the receiver takes it
	usleep(200000);
	pthread_sigmask(SIG_UNBLOCK, &set, NULL);
	pthread_create(&thread, NULL, receiver, NULL);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	// Regression: Darling's EVFILT_SIGNAL read a signalfd, which only sees a signal that every thread blocks; one
	// another thread took was lost. And the exited main thread took it without being able to handle it, which
	// aborted the process ("dserver_rpc_interrupt_enter failed").
	signal(SIG, SIG_IGN);
	ok = change(kq, EV_ADD);
	kill(getpid(), SIG);
	data = wait_signal(kq, 10000);
	check(ok && data == 1, "an ignored signal another thread takes is recorded", "no event");

	// (from here on, every signal is handled, so each check knows when it has come)
	signal(SIG, handler);
	kill(getpid(), SIG);
	data = wait_signal(kq, 10000);
	check(data == 1 && wait_handled(1), "a handled signal is recorded and handled", "no event, or the handler didn't run");

	// Each knote watching a signal records it, and deleting one leaves the others.
	ok = change(kq2, EV_ADD);
	kill(getpid(), SIG);
	ok = ok && wait_handled(2) && wait_signal(kq, 10000) == 1 && wait_signal(kq2, 10000) == 1;
	ok = ok && change(kq2, EV_DELETE);
	kill(getpid(), SIG);
	ok = ok && wait_handled(3) && wait_signal(kq, 10000) == 1 && wait_signal(kq2, 0) == -1;
	check(ok, "every knote for a signal records it, until deleted", "a knote missed it, or recorded it after EV_DELETE");

	// Regression: disabling a knote dropped it, so signals that arrived while libdispatch ran a source's handler
	// (EV_DISPATCH) were lost.
	ok = change(kq, EV_DELETE) && change(kq, EV_ADD | EV_DISPATCH);
	kill(getpid(), SIG);
	ok = ok && wait_handled(4) && wait_signal(kq, 10000) == 1;
	// one at a time, or Linux merges the second into the first while it's pending (XNU would count both)
	kill(getpid(), SIG);
	ok = ok && wait_handled(5);
	kill(getpid(), SIG);
	ok = ok && wait_handled(6) && wait_signal(kq, 0) == -1;
	ok = ok && change(kq, EV_ENABLE);
	data = wait_signal(kq, 10000);
	snprintf(detail, sizeof(detail), "%ld after EV_ENABLE", data);
	check(ok && data == 2, "a dispatched knote counts the signal until enabled again", detail);

	ok = change(kq, EV_DELETE);
	kill(getpid(), SIG);
	check(ok && wait_handled(7) && wait_signal(kq, 0) == -1, "a deleted knote records nothing", "an event after EV_DELETE");

	// A fork's child doesn't count its signals on its parent's knotes.
	change(kq, EV_ADD);
	pid = fork();
	if (pid == 0)
	{
		// its only thread takes it
		pthread_sigmask(SIG_UNBLOCK, &set, NULL);
		kill(getpid(), SIG);
		_exit(0);
	}
	waitpid(pid, NULL, 0);
	check(wait_signal(kq, 200) == -1, "a fork's child's signal isn't its parent's", "the parent's knote fired");

	exit(failures);
	return NULL;
}

int main(void)
{
	pthread_t thread;
	sigset_t set;

	// until it has exited: the threads inherit this, and run() unblocks it
	sigemptyset(&set);
	sigaddset(&set, SIG);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	pthread_create(&thread, NULL, run, NULL);
	pthread_exit(NULL);
}
