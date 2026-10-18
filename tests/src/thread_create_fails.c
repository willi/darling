// pthread_create() fails with EAGAIN when the thread can't be started, as on XNU (bsdthread_create fails with
// ENOMEM, and libpthread returns EAGAIN). mldr starts a Darwin thread on a Linux one; when that failed, the creating
// thread waited forever for the new thread to start.
//
// A child process lowers its address-space limit to what it uses plus HEADROOM: enough for the Darwin thread's own
// stack (libpthread's 512 KiB), not for the Linux thread's (8 MiB). Its pthread_create() must then return, within
// WAIT_MS. It uses only XNU's interface, plus Linux's /proc for the process's size.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define HEADROOM (4 << 20)
#define WAIT_MS 10000

static void* idle(void* arg)
{
	return arg;
}

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

// in the child: what it reports to the parent
struct result {
	int room;   // a mapping the size of a Darwin thread's stack still fits
	int err;    // pthread_create()'s result
};

static void child(int out)
{
	struct result r = { 0, -1 };
	unsigned long long size = vm_size();
	struct rlimit lim = { size + HEADROOM, RLIM_INFINITY };
	pthread_t thread;

	if (size == 0 || setrlimit(RLIMIT_AS, &lim) != 0)
		_exit(2);
	void* p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	r.room = p != MAP_FAILED;
	if (r.room)
		munmap(p, 1 << 20);
	r.err = pthread_create(&thread, NULL, idle, NULL);
	if (r.err == 0)
		pthread_join(thread, NULL);
	write(out, &r, sizeof(r));
	_exit(0);
}

int main(void)
{
	int failed = 0, fds[2], status;
	struct result r;
	pthread_t thread;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (pipe(fds) != 0)
	{
		printf("not ok - set up: pipe\n");
		return 1;
	}

	pid_t pid = fork();
	if (pid == 0)
	{
		close(fds[0]);
		child(fds[1]);
	}
	close(fds[1]);

	struct pollfd p = { .fd = fds[0], .events = POLLIN };
	int answered = poll(&p, 1, WAIT_MS) == 1 && read(fds[0], &r, sizeof(r)) == (ssize_t) sizeof(r);
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);

	if (!answered)
	{
		if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
			printf("not ok - set up: no address-space size or limit\n");
		else
			printf("not ok - pthread_create() returns when the thread can't start: no answer in %d ms\n", WAIT_MS);
		return 1;
	}

	printf("%s - pthread_create() fails with EAGAIN when the thread can't start: %s\n",
		r.err == EAGAIN ? "ok" : "not ok", r.err == 0 ? "it started" : strerror(r.err));
	if (r.err != EAGAIN)
		failed++;
	// else the check above may only have hit libpthread's own allocation
	printf("%s - there was room for a Darwin thread's stack\n", r.room ? "ok" : "not ok");
	if (!r.room)
		failed++;

	// and this process, without the limit, still starts threads
	int err = pthread_create(&thread, NULL, idle, NULL);
	if (err == 0)
		pthread_join(thread, NULL);
	printf("%s - a thread starts without the limit\n", err == 0 ? "ok" : "not ok");
	if (err != 0)
		failed++;
	return failed;
}
