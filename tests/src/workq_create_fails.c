// A workqueue thread request that no thread could be started for is served once threads can start again, as on XNU,
// which keeps it and retries (workq_schedule_delayed_thread_creation()). Each requester marks a thread as on its way
// and asks for none again until it comes: libdispatch's root queue (dgq_pending), the kevent workqueue's manager, a
// workloop's servicer. Darling dropped such a request, so that queue never ran again.
//
// Each case runs in a child process that hasn't started a thread (a fork child inherits glibc's cache of ended
// threads' stacks, and a cached stack would start a thread under the limit). The child sets up the workqueue as
// libdispatch does, lowers its address-space limit to what it uses plus some headroom, and asks for a thread, which
// can't start. It raises the limit again and asks for another thread, of another QoS: both must come.
// - "glibc's stack": room for mldr's 512 KiB stack, not for glibc's 8 MiB one (WQOPS_QUEUE_REQTHREADS);
// - "mldr's stack": not even room for mldr's. mldr didn't check that mapping and the process crashed;
// - "kevent manager": the thread is the kevent workqueue's manager, which an EVFILT_USER registration asks for; the
//   event it was triggered with must reach the kevent callback (workq_thread_request()).
//   Darling's first kqueue needs more than the headroom, so the child makes one before the limit.
//
// It uses only XNU's interface (as workq_kevent does), plus Linux's /proc for the process's size.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#define PRIVATE 1
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <pthread.h>
#include <pthread/qos_private.h>
#include <pthread/workqueue_private.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WAIT_MS 10000
// how long a thread that could start would take at most
#define NO_THREAD_MS 500
#define USER_IDENT 0x7177

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
// what the callbacks have seen
static bool seen_qos[QOS_CLASS_USER_INTERACTIVE + 1];
static bool seen_user_event;

static void worker_cb(pthread_priority_t priority)
{
	qos_class_t qos = _pthread_qos_class_decode(priority, NULL, NULL);

	pthread_mutex_lock(&lock);
	if (qos >= 0 && qos <= QOS_CLASS_USER_INTERACTIVE)
		seen_qos[qos] = true;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&lock);
}

static void kevent_cb(void** events, int* nevents)
{
	struct kevent_qos_s* list = *events;

	pthread_mutex_lock(&lock);
	for (int i = 0; i < *nevents; i++)
	{
		if (list[i].filter == EVFILT_USER && list[i].ident == USER_IDENT)
			seen_user_event = true;
	}
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&lock);
	*nevents = 0;
}

static void workloop_cb(uint64_t* workloop_id, void** events, int* nevents)
{
	*nevents = 0;
}

// waits up to `ms` for what `seen` answers; answers it
static bool wait_for(bool (*seen)(void), int ms)
{
	struct timespec deadline;
	bool done;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += ms / 1000;
	deadline.tv_nsec += (ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L)
	{
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&lock);
	while (!(done = seen()) && pthread_cond_timedwait(&cond, &lock, &deadline) != ETIMEDOUT)
		;
	done = seen();
	pthread_mutex_unlock(&lock);
	return done;
}

static bool seen_utility(void) { return seen_qos[QOS_CLASS_UTILITY]; }
static bool seen_both(void) { return seen_qos[QOS_CLASS_UTILITY] && seen_qos[QOS_CLASS_USER_INITIATED]; }
static bool seen_event(void) { return seen_user_event; }
static bool seen_event_and_worker(void) { return seen_user_event && seen_qos[QOS_CLASS_USER_INITIATED]; }

// the process's address-space size, from Linux's /proc; 0 if unknown
static unsigned long long vm_size(void)
{
	char buf[128] = { 0 };
	unsigned long long pages = 0;
	int fd = open("/proc/self/statm", O_RDONLY);

	if (fd < 0)
		return 0;
	if (read(fd, buf, sizeof(buf) - 1) > 0)
		pages = strtoull(buf, NULL, 10);
	close(fd);
	return pages * getpagesize();
}

static int user_event(uint16_t flags, uint32_t fflags)
{
	struct kevent_qos_s change = { .ident = USER_IDENT, .filter = EVFILT_USER, .flags = flags, .fflags = fflags };
	struct kevent_qos_s error;
	int rv = kevent_qos(-1, &change, 1, &error, 1, NULL, NULL, KEVENT_FLAG_WORKQ | KEVENT_FLAG_ERROR_EVENTS);

	if (rv < 0)
		return errno;
	return (rv > 0 && (error.flags & EV_ERROR)) ? (int) error.data : 0;
}

static int check(bool ok, const char* name, const char* detail)
{
	printf("%s - %s%s%s\n", ok ? "ok" : "not ok", name, ok ? "" : ": ", ok ? "" : detail);
	return ok ? 0 : 1;
}

// in the child; its exit status is its number of failed checks
static int child(const char* name, unsigned long long headroom, bool manager)
{
	struct pthread_workqueue_config config = {
		.version = PTHREAD_WORKQUEUE_CONFIG_VERSION,
		.kevent_cb = kevent_cb,
		.workloop_cb = workloop_cb,
		.workq_cb = worker_cb,
	};
	char what[160];
	int failed = 0, rv;

	alarm(3 * WAIT_MS / 1000);
	rv = pthread_workqueue_setup(&config, sizeof(config));
	if (rv != 0)
		return check(false, name, "pthread_workqueue_setup failed");
	// Darling's first kqueue needs more memory than the headroom, and the process ends without it: before the limit
	if (manager && kqueue() < 0)
		return check(false, name, "kqueue failed");

	unsigned long long size = vm_size();
	struct rlimit lim = { size + headroom, RLIM_INFINITY }, none = { RLIM_INFINITY, RLIM_INFINITY };
	if (size == 0 || setrlimit(RLIMIT_AS, &lim) != 0)
		return check(false, name, "no address-space size or limit");

	if (manager)
	{
		rv = user_event(EV_ADD | EV_CLEAR, 0);
		if (rv == 0)
			rv = user_event(0, NOTE_TRIGGER);
	}
	else
		rv = _pthread_workqueue_addthreads(1, _pthread_qos_class_encode(QOS_CLASS_UTILITY, 0, 0));
	bool early = wait_for(manager ? seen_event : seen_utility, NO_THREAD_MS);
	setrlimit(RLIMIT_AS, &none);

	snprintf(what, sizeof(what), "%s: the thread can't start under the limit", name);
	failed += check(rv == 0 && !early, what, (rv != 0) ? strerror(rv) : "it started: the test proves nothing");

	// another request, which may start a thread: the one before is served first, or with it
	rv = _pthread_workqueue_addthreads(1, _pthread_qos_class_encode(QOS_CLASS_USER_INITIATED, 0, 0));
	snprintf(what, sizeof(what), "%s: the request is served once a thread can start", name);
	failed += check(rv == 0 && wait_for(manager ? seen_event_and_worker : seen_both, WAIT_MS), what,
		(rv != 0) ? strerror(rv) : "no answer");
	return failed;
}

static int run(const char* name, unsigned long long headroom, bool manager)
{
	int status;

	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0)
	{
		int failed = child(name, headroom, manager);
		fflush(stdout);
		_exit(failed);
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid)
		return check(false, name, "fork");
	if (WIFSIGNALED(status))
	{
		printf("not ok - %s: the process died of signal %d%s\n", name, WTERMSIG(status),
			(WTERMSIG(status) == SIGALRM) ? " (timed out)" : "");
		return 1;
	}
	return WEXITSTATUS(status);
}

int main(void)
{
	int failed = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	// none of these may start a thread in this process (see the top)
	failed += run("glibc's stack", 4 << 20, false);
	failed += run("mldr's stack", 128 << 10, false);
	failed += run("kevent manager", 4 << 20, true);
	return failed;
}
