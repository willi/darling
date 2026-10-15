// Two syscalls libdispatch relies on, as XNU behaves:
//
// - __ulock_wait() returns how many other threads still wait on the address. Darling's used to always return 1, and
//   libdispatch traps on anything but 0 when the waiter was alone (backboardd died of SIGILL).
// - __kqueue_workloop_ctl() creates and destroys a workloop with a fixed priority. Darling had none, and libdispatch
//   then ran BaseBoard's fixed-priority workloop on a fallback queue where its timer source crashed (backboardd's
//   SIGSEGV).
//
// It only relies on XNU's interface, so it runs on macOS too. Prints one "ok"/"not ok" line per check; the exit status
// is the number of failed checks.

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define UL_COMPARE_AND_WAIT 1
#define ULF_NO_ERRNO 0x01000000

extern int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);

#define KQ_WORKLOOP_CREATE 0x01
#define KQ_WORKLOOP_DESTROY 0x02
#define KQ_WORKLOOP_CREATE_SCHED_PRI 0x01
#define KQ_WORKLOOP_CREATE_SCHED_POL 0x02
#define SCHED_RR 2

// struct kqueue_workloop_params, from XNU's sys/event_private.h
struct kqueue_workloop_params
{
	int kqwlp_version;
	int kqwlp_flags;
	uint64_t kqwlp_id;
	int kqwlp_sched_pri;
	int kqwlp_sched_pol;
	int kqwlp_cpu_percent;
	int kqwlp_cpu_refillms;
} __attribute__((packed));

extern int __kqueue_workloop_ctl(uintptr_t cmd, uint64_t options, void* addr, size_t sz);

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

static _Atomic uint32_t word;
static atomic_int waiting;
static int results[2];

static void* waiter(void* arg)
{
	int i = (int) (intptr_t) arg;
	atomic_fetch_add(&waiting, 1);
	results[i] = __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &word, 0, 0);
	return NULL;
}

// one thread at a time, so the count it returns is known
static void* lone_waiter(void* arg)
{
	(void) arg;
	atomic_fetch_add(&waiting, 1);
	results[0] = __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &word, 0, 0);
	return NULL;
}

static void wait_until_blocked(int count)
{
	while (atomic_load(&waiting) < count)
		usleep(1000);
	// the thread has announced itself; give it time to block in the kernel
	usleep(200 * 1000);
}

static int params_create(uint64_t id, int pri)
{
	struct kqueue_workloop_params p = { .kqwlp_version = sizeof(p),
		.kqwlp_flags = KQ_WORKLOOP_CREATE_SCHED_PRI | KQ_WORKLOOP_CREATE_SCHED_POL,
		.kqwlp_id = id,
		.kqwlp_sched_pri = pri,
		.kqwlp_sched_pol = SCHED_RR };
	return __kqueue_workloop_ctl(KQ_WORKLOOP_CREATE, 0, &p, sizeof(p));
}

static int params_destroy(uint64_t id)
{
	struct kqueue_workloop_params p = { .kqwlp_version = sizeof(p), .kqwlp_id = id };
	return __kqueue_workloop_ctl(KQ_WORKLOOP_DESTROY, 0, &p, sizeof(p));
}

int main(void)
{
	pthread_t t[2];

	// a lone waiter, woken: no one else waits
	pthread_create(&t[0], NULL, lone_waiter, NULL);
	wait_until_blocked(1);
	word = 1;
	__ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &word, 0);
	pthread_join(t[0], NULL);
	check(results[0] == 0, "a lone waiter's wait returns 0");

	// two waiters, woken one at a time: the first sees the other still waiting
	word = 0;
	atomic_store(&waiting, 0);
	for (int i = 0; i < 2; i++)
		pthread_create(&t[i], NULL, waiter, (void*) (intptr_t) i);
	wait_until_blocked(2);
	__ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &word, 0);
	usleep(200 * 1000);
	__ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &word, 0);
	for (int i = 0; i < 2; i++)
		pthread_join(t[i], NULL);
	check((results[0] == 1 && results[1] == 0) || (results[0] == 0 && results[1] == 1),
		"of two waiters woken in turn, the first returns 1 and the second 0");

	// a value that already changed: no wait, and no one waits
	check(__ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &word, 12345, 0) == 0, "a stale value returns 0 at once");

	uint64_t id = 0x5eed0000 + getpid();
	check(params_create(id, 47) == 0, "a fixed-priority workloop is created");
	errno = 0;
	check(params_create(id, 47) == -1 && errno == EEXIST, "creating it again fails with EEXIST");
	errno = 0;
	check(params_create(id + 1, 0) == -1 && errno == EINVAL, "priority 0 is refused");
	check(params_destroy(id) == 0, "it is destroyed");
	errno = 0;
	check(params_destroy(id) == -1 && errno == ENOENT, "destroying it again fails with ENOENT");

	return failures;
}
