// libkqueue's walk over the live kqueues (willi/darling-libkqueue: kqueue_foreach(), kqueue_free(), kqueue_atfork()).
// Every close() of a descriptor walks the process's kqueues to delete the descriptor's knotes. That walk used to scan
// a table with a slot for every descriptor RLIMIT_NOFILE's hard limit allows (1,048,576 in a Docker container), about
// 1 ms a close(), which was most of lsd's CPU on a device's first boot. It now walks a list of the live kqueues.
// The checks: the walk still reaches every kqueue (one reached only through a dup() too), a kqueue's descriptors go
// with it, a fork's child has none of them, the walk survives kqueues being created and closed while it runs, and a
// close() costs about the same with a kqueue in the process as before the first one. It only relies on XNU's
// interface, so it runs on macOS too.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <sys/event.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

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

static uint64_t now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t) tv.tv_sec * 1000000 + tv.tv_usec;
}

// the least time, in microseconds, that 200 open()/close() pairs of /dev/null took, over 10 tries
static uint64_t close_cost(void)
{
	uint64_t best = UINT64_MAX;

	for (int round = 0; round < 10; round++)
	{
		uint64_t start = now_us();
		for (int i = 0; i < 200; i++)
		{
			int fd = open("/dev/null", O_RDONLY);
			if (fd >= 0)
				close(fd);
		}
		uint64_t took = now_us() - start;
		if (took < best)
			best = took;
	}
	return best;
}

static bool watch_read(int kq, int fd)
{
	struct kevent64_s kev;

	EV_SET64(&kev, fd, EVFILT_READ, EV_ADD, 0, 0, 0, 0, 0);
	return kevent64(kq, &kev, 1, NULL, 0, 0, NULL) == 0;
}

// whether kq still has an EVFILT_READ knote for fd: deleting one succeeds only if it does
static bool has_read_knote(int kq, int fd)
{
	struct kevent64_s kev;

	EV_SET64(&kev, fd, EVFILT_READ, EV_DELETE, 0, 0, 0, 0, 0);
	return kevent64(kq, &kev, 1, NULL, 0, 0, NULL) == 0;
}

// whether kq is a working kqueue: a 1 ms timer on it fires
static bool works(int kq)
{
	struct kevent64_s kev;
	struct timespec timeout = { 2, 0 };

	EV_SET64(&kev, 7, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 1, 0, 0, 0);
	if (kevent64(kq, &kev, 1, NULL, 0, 0, NULL) != 0)
		return false;
	return kevent64(kq, NULL, 0, &kev, 1, 0, &timeout) == 1 && kev.filter == EVFILT_TIMER;
}

static bool is_kqueue(int fd)
{
	struct timespec zero = { 0, 0 };
	struct kevent64_s kev;

	return kevent64(fd, NULL, 0, &kev, 1, 0, &zero) >= 0;
}

// whether a fork's child still has fd from its parent: a kqueue isn't inherited, so the descriptor is closed
static bool inherited(int fd)
{
	return fcntl(fd, F_GETFD) != -1 || is_kqueue(fd);
}

static volatile bool stop;

// creates kqueues with a knote each and closes them, over and over, while the main thread's close()s walk them
static void* churn(void* arg)
{
	long* rounds = arg;
	int p[2];

	if (pipe(p) != 0)
		return NULL;
	while (!stop)
	{
		int kq = kqueue();
		if (kq < 0)
			break;
		watch_read(kq, p[0]);
		close(kq);
		++*rounds;
	}
	close(p[0]);
	close(p[1]);
	return NULL;
}

int main(void)
{
	char detail[160];
	struct rlimit rl;

	// before the process has a kqueue, a close() walks nothing at all
	uint64_t before = close_cost();
	int first = kqueue();
	uint64_t after = close_cost();
	getrlimit(RLIMIT_NOFILE, &rl);
	snprintf(detail, sizeof(detail), "200 closes: %llu us before the first kqueue, %llu us with one (hard limit %llu)",
		(unsigned long long) before, (unsigned long long) after, (unsigned long long) rl.rlim_max);
	printf("# %s\n", detail);
	check(first >= 0 && after < 4 * before + 2000, "a close() costs about the same with a kqueue", detail);

	// close() deletes the descriptor's knotes in every kqueue, one reached only through a dup() too
	int p[2];
	int a = kqueue(), b = kqueue(), c = kqueue();
	int d = dup(c);
	close(c);
	bool set = pipe(p) == 0 && watch_read(a, p[0]) && watch_read(b, p[0]) && watch_read(d, p[0]) &&
		watch_read(first, p[0]);
	check(set, "knotes on a pipe in four kqueues", strerror(errno));
	close(p[0]);
	snprintf(detail, sizeof(detail), "knote left in:%s%s%s%s", has_read_knote(a, p[0]) ? " a" : "",
		has_read_knote(b, p[0]) ? " b" : "", has_read_knote(d, p[0]) ? " the dup" : "",
		has_read_knote(first, p[0]) ? " the first" : "");
	check(strcmp(detail, "knote left in:") == 0, "close() deletes the knotes in every kqueue", detail);
	close(p[1]);

	// a kqueue reached only through its dup works; once that is closed too, neither descriptor is a kqueue
	check(works(d), "a kqueue works through its dup after the original is closed", "its timer didn't fire");
	close(d);
	check(!is_kqueue(d) && !is_kqueue(c), "a kqueue's descriptors go with it", "kevent() still took one");

	// a fork's child has none of the parent's kqueues, its own work, and its close()s still walk them
	int e = dup(a);
	pid_t pid = fork();
	if (pid == 0)
	{
		int bad = inherited(a) + inherited(e) + inherited(b) + inherited(first);
		int kq = kqueue();
		int q[2];
		bool ok = pipe(q) == 0 && watch_read(kq, q[0]) && works(kq);
		close(q[0]);
		ok = ok && !has_read_knote(kq, q[0]);
		_exit(bad * 2 + !ok);
	}
	int status = -1;
	waitpid(pid, &status, 0);
	snprintf(detail, sizeof(detail),
		"the child's status %d (2 for each parent kqueue descriptor it had, +1 if its own kqueue failed)",
		WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a fork's child has its own kqueues only", detail);
	check(works(a) && works(e), "the parent's kqueues work after the fork", "a timer didn't fire");

	// close()s walking the kqueues while other threads create and close them
	pthread_t threads[4];
	long rounds[4] = { 0 };
	long closes = 0;
	int n = 0;
	for (; n < 4; n++)
		if (pthread_create(&threads[n], NULL, churn, &rounds[n]) != 0)
			break;
	uint64_t end = now_us() + 2000000;
	while (now_us() < end)
	{
		int fd = open("/dev/null", O_RDONLY);
		if (fd >= 0 && close(fd) == 0)
			closes++;
	}
	stop = true;
	long created = 0;
	for (int i = 0; i < n; i++)
	{
		pthread_join(threads[i], NULL);
		created += rounds[i];
	}
	snprintf(detail, sizeof(detail), "%d threads, %ld kqueues created and closed, %ld closes", n, created, closes);
	printf("# %s\n", detail);
	check(n == 4 && created > 0 && closes > 0 && works(a) && works(b), "close() while kqueues come and go", detail);

	return failures;
}
