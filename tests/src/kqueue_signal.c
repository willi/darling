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

static int change(int kq, uint16_t flags)
{
	struct kevent kev;

	EV_SET(&kev, SIG, EVFILT_SIGNAL, flags, 0, 0, NULL);
	return kevent(kq, &kev, 1, NULL, 0, NULL);
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
	int kq = kqueue();

	(void) arg;
	pthread_create(&thread, NULL, receiver, NULL);
	sigemptyset(&set);
	sigaddset(&set, SIG);

	// Regression: Darling's EVFILT_SIGNAL read a signalfd, which only sees a signal that every thread blocks; one
	// another thread took was lost (and with the main thread exited, took the process down instead).
	signal(SIG, SIG_IGN);
	change(kq, EV_ADD);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	kill(getpid(), SIG);
	data = wait_signal(kq, 5000);
	check(data == 1, "an ignored signal another thread takes is recorded", "no event");

	signal(SIG, handler);
	kill(getpid(), SIG);
	data = wait_signal(kq, 5000);
	// the knote fires before the handler runs, on the thread that took the signal
	for (int i = 0; i < 500 && handled == 0; i++)
		usleep(10000);
	check(data == 1 && handled == 1, "a handled signal is recorded and handled", "no event, or the handler didn't run");

	// Regression: disabling a knote dropped it, so signals that arrived while libdispatch ran a source's handler
	// (EV_DISPATCH) were lost.
	signal(SIG, SIG_IGN);
	change(kq, EV_ADD | EV_DISPATCH);
	kill(getpid(), SIG);
	data = wait_signal(kq, 5000);
	// apart, or Linux merges the second into the first while it's pending (XNU would count both)
	kill(getpid(), SIG);
	usleep(100000);
	kill(getpid(), SIG);
	usleep(100000);
	change(kq, EV_ENABLE);
	data = wait_signal(kq, 5000);
	snprintf(detail, sizeof(detail), "%ld after EV_ENABLE", data);
	check(data == 2, "a disabled knote counts the signal until enabled", detail);

	change(kq, EV_DELETE);
	kill(getpid(), SIG);
	check(wait_signal(kq, 200) == -1, "a deleted knote records nothing", "an event after EV_DELETE");

	// A fork's child shares its parent's knote descriptors until it execs, but its signals aren't the parent's.
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

	pthread_create(&thread, NULL, run, NULL);
	pthread_exit(NULL);
}
