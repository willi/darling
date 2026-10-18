// A kprintf (libsystem_kernel's __simple_kprintf, Darling's debug log to darlingserver) costs no darlingserver call
// when darlingserver doesn't log it, as at its default log level. Each was a round trip whose string darlingserver then
// dropped; on a signal path, 76 of them per signal made a livelock.
//
// The fastest of BATCHES batches of ROUNDS kprintfs must cost at most MAX_PAIRS of a pair of Mach calls
// (mach_thread_self() and mach_port_deallocate(), darlingserver calls too), timed in alternating batches; a kprintf
// that made its call measures about half a pair.
//
// When darlingserver logs kprintf strings (DSERVER_LOG_LEVEL info or below, or "kprintf" in DSERVER_LOG_CATEGORIES),
// the calls are made, as they must be: the test asks darlingserver itself (kprintf_logged, from a child of its own,
// on a socket of its own, as dserver_bad_push speaks the protocol) and skips the check.
//
// What it can't see: the fallback when darlingserver doesn't know kprintf_logged (that needs an older darlingserver),
// and a fork child keeping the answer (one more query falls in a batch, and the fastest batch hides it).
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <darlingserver/rpc.h>
#include <mach/mach.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BATCHES 20
#define ROUNDS 10
#define MAX_PAIRS 0.1
#define SERVER_SOCKET "/.darlingserver.sock"
#define WAIT_MS 5000

// libsystem_kernel's; not in any header
extern void __simple_kprintf(const char* format, ...);

static int compare(const void* a, const void* b)
{
	double x = *(const double*) a, y = *(const double*) b;
	return (x > y) - (x < y);
}

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int readable(int fd, int ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	return poll(&p, 1, ms) == 1;
}

// in a child: asks darlingserver kprintf_logged on a socket of its own (its thread's replies then go there, so the
// child makes no other call); '1' logged, '0' not, '?' no answer
static char ask_logged(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	dserver_rpc_call_kprintf_logged_t call = {
		.header = {
			.number = dserver_callnum_kprintf_logged,
			.pid = getpid(),
			.tid = (pid_t) syscall(SYS_thread_selfid),
			.architecture = dserver_rpc_architecture_x86_64,
		},
	};
	dserver_rpc_reply_kprintf_logged_t reply;
	int s = socket(AF_UNIX, SOCK_DGRAM, 0);

	if (s < 0)
		return '?';
	snprintf(addr.sun_path, sizeof(addr.sun_path), "/private/var/tmp/kprintf_cost.%d", getpid());
	unlink(addr.sun_path);
	if (bind(s, (struct sockaddr*) &addr, sizeof(addr)) != 0)
		return '?';
	struct sockaddr_un server = { .sun_family = AF_UNIX };
	strncpy(server.sun_path, SERVER_SOCKET, sizeof(server.sun_path) - 1);
	if (connect(s, (struct sockaddr*) &server, sizeof(server)) != 0 || send(s, &call, sizeof(call), 0) != sizeof(call))
		return '?';
	if (!readable(s, WAIT_MS) || recv(s, &reply, sizeof(reply), 0) != sizeof(reply) || reply.header.code != 0)
		return '?';
	unlink(addr.sun_path);
	return reply.body.logged ? '1' : '0';
}

// whether darlingserver logs kprintf strings: 1, 0, or -1 if it didn't say
static int kprintf_logged(void)
{
	int result[2];
	char c = '?';

	if (pipe(result) != 0)
		return -1;
	pid_t pid = fork();
	if (pid == 0)
	{
		c = ask_logged();
		write(result[1], &c, 1);
		for (;;)
			pause();
	}
	close(result[1]);
	if (!readable(result[0], 2 * WAIT_MS) || read(result[0], &c, 1) != 1)
		c = '?';
	close(result[0]);
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	return c == '1' ? 1 : c == '0' ? 0 : -1;
}

int main(void)
{
	double pairs[BATCHES], kprintfs[BATCHES];

	setvbuf(stdout, NULL, _IOLBF, 0);
	int logged = kprintf_logged();
	if (logged < 0)
	{
		printf("not ok - darlingserver says whether it logs kprintf strings: no answer\n");
		return 1;
	}
	if (logged)
	{
		printf("ok - an unlogged kprintf costs no darlingserver call # skip darlingserver logs kprintf strings\n");
		return 0;
	}

	for (int b = 0; b < BATCHES; b++)
	{
		double t0 = now_ms();
		for (int i = 0; i < ROUNDS; i++)
			mach_port_deallocate(mach_task_self(), mach_thread_self());
		double t1 = now_ms();
		for (int i = 0; i < ROUNDS; i++)
			__simple_kprintf("kprintf_cost: %d.%d\n", b, i);
		double t2 = now_ms();
		pairs[b] = (t1 - t0) / ROUNDS;
		kprintfs[b] = (t2 - t1) / ROUNDS;
	}
	qsort(pairs, BATCHES, sizeof(double), compare);
	qsort(kprintfs, BATCHES, sizeof(double), compare);

	double pair = pairs[0], kprintf = kprintfs[0];
	int ok = kprintf <= MAX_PAIRS * pair;
	printf("%s - an unlogged kprintf costs %.3f Mach call pairs (at most %.1f): %.4f ms vs %.3f ms\n",
		ok ? "ok" : "not ok", kprintf / pair, MAX_PAIRS, kprintf, pair);
	return !ok;
}
