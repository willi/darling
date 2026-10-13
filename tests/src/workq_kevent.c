// Exercises the kernel side of Apple's libdispatch without libdispatch: the kevent workqueue (kevent_qos() with
// KEVENT_FLAG_WORKQ), workloops (kevent_id()), the workqueue threads that service both, and fork() with live
// kqueues. Darling's own libdispatch uses none of these, so this is what runs that code outside an iOS
// Simulator runtime. It only relies on XNU's interface, so it runs on macOS too. It also checks that
// proc_pidinfo() agrees with the audit token and the parent about the process's identity, which these consume.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#define PRIVATE 1
#include <sys/event.h>
#include <sys/proc_info.h>
#include <sys/wait.h>
#include <pthread.h>
#include <pthread/qos_private.h>
#include <pthread/workqueue_private.h>
#include <errno.h>
#include <libproc.h>
#include <mach/mach.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WAIT_SECONDS 10
// enough servicer re-entries to overflow a workqueue thread's 512 KiB stack if each leaked a frame
#define HANDOFFS 20000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int failures;

// what the callbacks have seen
static struct kevent_qos_s seen[256];
static uint64_t seen_workloop[256]; // 0 for the kevent workqueue
static int nseen;
static int workloop_batches;

// what the callbacks hand back
static struct kevent_qos_s manager_reply;
static bool manager_reply_pending;
static int handoffs_left;
static struct kevent_qos_s handoff_change;

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

static void record(uint64_t workloop, const struct kevent_qos_s* events, int nevents)
{
	int i;

	for (i = 0; i < nevents && nseen < 256; i++)
	{
		seen[nseen] = events[i];
		seen_workloop[nseen] = workloop;
		nseen++;
	}
	pthread_cond_broadcast(&cond);
}

static void worker_cb(pthread_priority_t priority)
{
}

static void kevent_cb(void** events, int* nevents)
{
	struct kevent_qos_s* list = *events;

	pthread_mutex_lock(&lock);
	record(0, list, *nevents);
	*nevents = 0;
	if (manager_reply_pending)
	{
		list[0] = manager_reply;
		*nevents = 1;
		manager_reply_pending = false;
	}
	pthread_mutex_unlock(&lock);
}

static void workloop_cb(uint64_t* workloop_id, void** events, int* nevents)
{
	struct kevent_qos_s* list = *events;

	pthread_mutex_lock(&lock);
	workloop_batches++;
	// the handoff loop would flood `seen`
	if (handoffs_left == 0)
		record(*workloop_id, list, *nevents);
	*nevents = 0;
	if (handoffs_left > 0)
	{
		// touching the thread request again keeps this thread servicing the workloop
		list[0] = handoff_change;
		*nevents = 1;
		handoffs_left--;
	}
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&lock);
}

// Waits until the callbacks have seen an event matching `filter` and `ident` on `workloop`; returns its index.
static int wait_event(uint64_t workloop, int16_t filter, uint64_t ident, uint16_t flags)
{
	struct timespec deadline;
	int i, found = -1;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += WAIT_SECONDS;

	pthread_mutex_lock(&lock);
	for (;;)
	{
		for (i = 0; i < nseen; i++)
		{
			if (seen_workloop[i] == workloop && seen[i].filter == filter && seen[i].ident == ident
					&& (seen[i].flags & flags) == flags)
			{
				found = i;
				break;
			}
		}
		if (found >= 0 || pthread_cond_timedwait(&cond, &lock, &deadline) == ETIMEDOUT)
			break;
	}
	if (found < 0)
	{
		printf("# waited for workloop 0x%llx filter %d ident %llu; seen:\n", (unsigned long long)workloop, filter,
			(unsigned long long)ident);
		for (i = 0; i < nseen; i++)
			printf("#   workloop 0x%llx filter %d ident %llu flags 0x%x data %lld\n",
				(unsigned long long)seen_workloop[i], seen[i].filter, (unsigned long long)seen[i].ident,
				seen[i].flags, (long long)seen[i].data);
	}
	pthread_mutex_unlock(&lock);
	return found;
}

static void forget_events(void)
{
	pthread_mutex_lock(&lock);
	nseen = 0;
	pthread_mutex_unlock(&lock);
}

// Applies one change to the kevent workqueue; returns its error (0 if none was reported), or -errno.
static int workq_change(uint64_t ident, int16_t filter, uint16_t flags, uint32_t fflags, uint64_t udata)
{
	struct kevent_qos_s change = { .ident = ident, .filter = filter, .flags = flags, .fflags = fflags, .udata = udata };
	struct kevent_qos_s error;
	int rv = kevent_qos(-1, &change, 1, &error, 1, NULL, NULL, KEVENT_FLAG_WORKQ | KEVENT_FLAG_ERROR_EVENTS);

	if (rv < 0)
		return -errno;
	return (rv > 0 && (error.flags & EV_ERROR)) ? (int)error.data : 0;
}

static int workloop_change(uint64_t id, struct kevent_qos_s* change)
{
	struct kevent_qos_s error;
	int rv = kevent_id(id, change, 1, &error, 1, NULL, NULL, KEVENT_FLAG_WORKLOOP | KEVENT_FLAG_ERROR_EVENTS);

	if (rv < 0)
		return -errno;
	if (rv > 0 && (error.flags & EV_ERROR))
	{
		*change = error;
		return (int)error.data;
	}
	return 0;
}

static struct kevent_qos_s thread_request(uint64_t id)
{
	return (struct kevent_qos_s) {
		.ident = id,
		.filter = EVFILT_WORKLOOP,
		.flags = EV_ADD | EV_ENABLE,
		.fflags = NOTE_WL_THREAD_REQUEST,
		.qos = (int32_t)_pthread_qos_class_encode(QOS_CLASS_DEFAULT, 0, 0),
	};
}

static void test_kevent_workqueue(void)
{
	int i;

	check(workq_change(1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0x11) == 0, "workq: register", "failed");
	// a change without EV_UDATA_SPECIFIC replaces the udata
	check(workq_change(1, EVFILT_USER, 0, NOTE_TRIGGER, 0x11) == 0, "workq: trigger", "failed");
	i = wait_event(0, EVFILT_USER, 1, 0);
	check(i >= 0 && seen[i].udata == 0x11, "workq: the event manager gets the event", "no event");
}

// A knote that delivered an event and is EV_DISPATCH|EV_UDATA_SPECIFIC is only deleted once re-enabled.
static void test_deferred_delete(void)
{
	const uint16_t dispatch2 = EV_DISPATCH | EV_UDATA_SPECIFIC;
	int fds[2], rv;

	forget_events();
	pipe(fds);
	write(fds[1], "x", 1);
	workq_change(fds[0], EVFILT_READ, EV_ADD | dispatch2, 0, 0x22);
	check(wait_event(0, EVFILT_READ, fds[0], 0) >= 0, "deferdelete: event", "no event");

	rv = workq_change(fds[0], EVFILT_READ, EV_DELETE | dispatch2, 0, 0x22);
	check(rv == EINPROGRESS, "deferdelete: deleting a delivered knote is deferred", strerror(rv));

	rv = workq_change(fds[0], EVFILT_READ, EV_ENABLE | dispatch2, 0, 0x22);
	check(rv == 0, "deferdelete: re-enabling it", strerror(rv));
	check(wait_event(0, EVFILT_READ, fds[0], EV_DELETE) >= 0, "deferdelete: re-enabling delivers the deletion",
		"no EV_DELETE event");

	rv = workq_change(fds[0], EVFILT_READ, EV_DELETE | dispatch2, 0, 0x22);
	check(rv == ENOENT, "deferdelete: the knote is gone after that", strerror(rv));

	close(fds[0]);
	close(fds[1]);
}

// A change the event manager hands back that fails comes back to it as an event, as on XNU's parking path.
static void test_manager_errors(void)
{
	int i;

	forget_events();
	pthread_mutex_lock(&lock);
	manager_reply = (struct kevent_qos_s) { .ident = 999, .filter = EVFILT_USER, .flags = EV_DELETE };
	manager_reply_pending = true;
	pthread_mutex_unlock(&lock);

	workq_change(3, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0x33);
	workq_change(3, EVFILT_USER, 0, NOTE_TRIGGER, 0x33);
	i = wait_event(0, EVFILT_USER, 999, EV_ERROR);
	check(i >= 0 && seen[i].data == ENOENT, "workq: failed manager changes are redelivered", "no error event");
}

static void test_workloop_thread_request(void)
{
	const uint64_t id = 0x1000;
	struct kevent_qos_s tr = thread_request(id);
	struct timespec deadline;
	int rv, batches;

	forget_events();
	rv = workloop_change(id, &tr);
	check(rv == 0, "workloop: thread request", strerror(rv));
	check(wait_event(id, EVFILT_WORKLOOP, id, 0) >= 0, "workloop: a servicer gets the thread request", "none");

	// each touch from the servicer makes it re-enter the callback on the same thread
	pthread_mutex_lock(&lock);
	handoff_change = thread_request(id);
	handoffs_left = HANDOFFS;
	batches = workloop_batches;
	pthread_mutex_unlock(&lock);

	tr = thread_request(id);
	workloop_change(id, &tr);

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 6 * WAIT_SECONDS;
	pthread_mutex_lock(&lock);
	while (handoffs_left > 0 && pthread_cond_timedwait(&cond, &lock, &deadline) != ETIMEDOUT);
	batches = workloop_batches - batches;
	pthread_mutex_unlock(&lock);
	check(batches > HANDOFFS, "workloop: servicer re-entry doesn't exhaust its stack", "handoffs stalled");

	tr = thread_request(id);
	tr.flags = EV_DELETE;
	rv = workloop_change(id, &tr);
	check(rv == 0, "workloop: deleting the thread request", strerror(rv));
}

static void test_workloop_estale(void)
{
	const uint64_t id = 0x2000;
	uint64_t value = 5;
	struct kevent_qos_s tr = thread_request(id);
	int rv;

	tr.ext[EV_EXTIDX_WL_ADDR] = (uintptr_t)&value;
	tr.ext[EV_EXTIDX_WL_MASK] = ~0ull;
	tr.ext[EV_EXTIDX_WL_VALUE] = 6;
	rv = workloop_change(id, &tr);
	check(rv == ESTALE && tr.ext[EV_EXTIDX_WL_VALUE] == 5, "workloop: a stale debounce value fails with ESTALE",
		strerror(rv));
}

static volatile bool sync_waiter_woken;

static void* sync_waiter(void* arg)
{
	struct kevent_qos_s wait = {
		.ident = 0xa1, .filter = EVFILT_WORKLOOP, .flags = EV_ADD | EV_DISABLE, .fflags = NOTE_WL_SYNC_WAIT,
	};
	int rv = workloop_change((uintptr_t)arg, &wait);

	sync_waiter_woken = rv == 0;
	wait = (struct kevent_qos_s) {
		.ident = 0xa1, .filter = EVFILT_WORKLOOP, .flags = EV_DELETE, .fflags = NOTE_WL_SYNC_WAIT,
	};
	workloop_change((uintptr_t)arg, &wait);
	return NULL;
}

static void test_workloop_sync(void)
{
	const uint64_t id = 0x3000;
	struct kevent_qos_s wake = {
		.ident = 0xa1, .filter = EVFILT_WORKLOOP, .flags = EV_ADD | EV_DISABLE, .fflags = NOTE_WL_SYNC_WAKE,
	};
	pthread_t thread;
	int rv;

	pthread_create(&thread, NULL, sync_waiter, (void*)(uintptr_t)id);
	usleep(300 * 1000);
	check(!sync_waiter_woken, "workloop: NOTE_WL_SYNC_WAIT blocks", "it returned");

	rv = workloop_change(id, &wake);
	check(rv == 0, "workloop: NOTE_WL_SYNC_WAKE", strerror(rv));
	pthread_join(thread, NULL);
	check(sync_waiter_woken, "workloop: NOTE_WL_SYNC_WAKE wakes the waiter", "it failed");
}

static void test_workloop_knote(void)
{
	const uint64_t id = 0x4000;
	struct kevent_qos_s change = { .ident = 7, .filter = EVFILT_USER, .flags = EV_ADD | EV_CLEAR, .udata = 0x77 };
	int i, rv;

	forget_events();
	rv = workloop_change(id, &change);
	check(rv == 0, "workloop: registering an ordinary knote", strerror(rv));
	change = (struct kevent_qos_s) { .ident = 7, .filter = EVFILT_USER, .fflags = NOTE_TRIGGER, .udata = 0x77 };
	workloop_change(id, &change);
	i = wait_event(id, EVFILT_USER, 7, 0);
	check(i >= 0 && seen[i].udata == 0x77, "workloop: its event goes to the workloop's servicer", "no event");
}

static bool unique_identifiers(pid_t pid, struct proc_uniqidentifierinfo* info)
{
	return proc_pidinfo(pid, PROC_PIDUNIQIDENTIFIERINFO, 0, info, sizeof(*info)) == sizeof(*info);
}

static void test_identity(void)
{
	struct proc_uniqidentifierinfo info;
	audit_token_t token;
	mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
	bool ok = unique_identifiers(getpid(), &info)
		&& task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count) == KERN_SUCCESS;

	check(ok && info.p_uniqueid != 0, "identity: the process has a unique id", "none");
	check(ok && info.p_idversion != 0 && info.p_idversion == (int32_t)token.val[7],
		"identity: proc_pidinfo's pidversion is the audit token's", "they differ");
}

// The child of a fork has none of the parent's kqueues, and can use new ones.
static void test_fork(void)
{
	int kq = kqueue();
	struct proc_uniqidentifierinfo parent;
	pid_t child;
	int status = -1, i;

	unique_identifiers(getpid(), &parent);

	child = fork();
	if (child == 0)
	{
		struct kevent change, event;
		struct timespec timeout = { WAIT_SECONDS, 0 };
		int new_kq;

		alarm(WAIT_SECONDS);
		if (fcntl(kq, F_GETFD) != -1)
			_exit(2);
		new_kq = kqueue();
		EV_SET(&change, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
		kevent(new_kq, &change, 1, NULL, 0, NULL);
		EV_SET(&change, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
		kevent(new_kq, &change, 1, NULL, 0, NULL);
		free(malloc(64));
		if (kevent(new_kq, NULL, 0, &event, 1, &timeout) != 1)
			_exit(3);
		{
			struct proc_uniqidentifierinfo info;

			if (!unique_identifiers(getpid(), &info) || info.p_puniqueid != parent.p_uniqueid
					|| info.p_uniqueid == parent.p_uniqueid)
				_exit(4);
		}
		_exit(0);
	}

	for (i = 0; i < WAIT_SECONDS * 10; i++)
	{
		if (waitpid(child, &status, WNOHANG) == child)
			break;
		usleep(100 * 1000);
	}
	check(i < WAIT_SECONDS * 10, "fork: the child finishes", "it hangs");
	if (i == WAIT_SECONDS * 10)
		kill(child, SIGKILL);
	check(WIFEXITED(status) && WEXITSTATUS(status) != 2 && WEXITSTATUS(status) != 3,
		"fork: the child doesn't inherit kqueues and has new ones", "wrong exit status");
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork: the child's parent unique id is the parent's unique id",
		"wrong exit status");

	// the parent's kqueues still work
	forget_events();
	workq_change(1, EVFILT_USER, 0, NOTE_TRIGGER, 0x11);
	check(wait_event(0, EVFILT_USER, 1, 0) >= 0, "fork: the parent's kevent workqueue still works", "no event");
	close(kq);
}

int main(void)
{
	struct pthread_workqueue_config config = {
		.version = PTHREAD_WORKQUEUE_CONFIG_VERSION,
		.kevent_cb = kevent_cb,
		.workloop_cb = workloop_cb,
		.workq_cb = worker_cb,
	};
	int rv;

	alarm(30 * WAIT_SECONDS);

	rv = pthread_workqueue_setup(&config, sizeof(config));
	check(rv == 0, "pthread_workqueue_setup", strerror(rv));
	if (rv != 0)
		return failures;

	test_kevent_workqueue();
	test_deferred_delete();
	test_manager_errors();
	test_workloop_thread_request();
	test_workloop_estale();
	test_workloop_sync();
	test_workloop_knote();
	test_identity();
	test_fork();

	return failures;
}
