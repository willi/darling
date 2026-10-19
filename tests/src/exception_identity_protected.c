// EXCEPTION_IDENTITY_PROTECTED (xnu-8019 on): an exception port that gets the thread's ID and a task identity token,
// never its thread or task port. Spotlight's searchd sets one on each of its threads (SpotlightIndex's
// ContentIndexExceptionHandler.c), and asserted when Darling refused it with KERN_INVALID_ARGUMENT; it died within a
// second of every start.
//
// Each of thread/task set/swap_exception_ports takes the behavior with MACH_EXCEPTION_CODES and refuses it without
// (XNU's osfmk/kern/ipc_tt.c). A fault then comes to the port as mach_exception_raise_identity_protected (XNU's
// exception_deliver()), whose token gives back the task's port, and the thread goes on when the handler answers
// KERN_SUCCESS. The same happens with EXCEPTION_DEFAULT first, so a failure of delivery itself shows as such.
// It only relies on XNU's interface, so it runs on macOS too.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <mach/mach.h>
#include <mach/exception_types.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef EXCEPTION_IDENTITY_PROTECTED
#define EXCEPTION_IDENTITY_PROTECTED 4
#endif

// mach_exc's message IDs (osfmk/mach/mach_exc.defs: subsystem 2405, one per routine)
#define MSGID_RAISE 2405
#define MSGID_RAISE_IDENTITY_PROTECTED 2408

static int failures;

static void check(bool ok, const char* what, const char* detail)
{
	if (ok)
		printf("ok - %s\n", what);
	else
	{
		printf("not ok - %s: %s\n", what, detail);
		failures++;
	}
	fflush(stdout);
}

// write(), not printf(): stdout may be a file, fully buffered, which _exit() drops
static void hung(int sig)
{
	static const char line[] = "not ok - the faulting thread went on within 30 s\n";

	(void) sig;
	write(STDOUT_FILENO, line, sizeof(line) - 1);
	_exit(1);
}

#pragma pack(4)
// mach_exception_raise's request, as MIG lays it out
typedef struct {
	mach_msg_header_t head;
	mach_msg_body_t body;
	mach_msg_port_descriptor_t thread;
	mach_msg_port_descriptor_t task;
	NDR_record_t ndr;
	exception_type_t exception;
	mach_msg_type_number_t code_count;
	int64_t code[2];
} raise_request_t;

// mach_exception_raise_identity_protected's
typedef struct {
	mach_msg_header_t head;
	mach_msg_body_t body;
	mach_msg_port_descriptor_t task_id_token;
	NDR_record_t ndr;
	uint64_t thread_id;
	exception_type_t exception;
	mach_msg_type_number_t code_count;
	int64_t code[2];
} raise_protected_request_t;

typedef struct {
	mach_msg_header_t head;
	NDR_record_t ndr;
	kern_return_t ret_code;
} raise_reply_t;
#pragma pack()

typedef union {
	mach_msg_header_t head;
	raise_request_t raise;
	raise_protected_request_t protected_raise;
	char space[1024];
} request_t;

// what the faulting thread was, and what came to the port
static mach_port_t exc_port;
static int behavior;
static char* page;
static uint64_t faulting_thread_id;
static volatile bool went_on;

static uint64_t thread_id_of(thread_act_t thread)
{
	thread_identifier_info_data_t info;
	mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;

	if (thread_info(thread, THREAD_IDENTIFIER_INFO, (thread_info_t) &info, &count) != KERN_SUCCESS)
		return 0;
	return info.thread_id;
}

static void* faulter(void* arg)
{
	thread_act_t self = mach_thread_self();
	(void) arg;

	faulting_thread_id = thread_id_of(self);
	if (thread_set_exception_ports(self, EXC_MASK_BAD_ACCESS, exc_port, behavior, THREAD_STATE_NONE) == KERN_SUCCESS)
	{
		page[0] = 1; // EXC_BAD_ACCESS, until the handler makes the page writable
		went_on = true;
	}
	mach_port_deallocate(mach_task_self(), self);
	return NULL;
}

// Runs a thread that faults with `behavior` set on it, and answers its exception; returns the request's ID
static mach_msg_id_t fault_and_answer(int with_behavior, request_t* request)
{
	pthread_t thread;
	raise_reply_t reply;
	mach_msg_return_t mr;

	behavior = with_behavior;
	went_on = false;
	mprotect(page, getpagesize(), PROT_NONE);
	pthread_create(&thread, NULL, faulter, NULL);

	memset(request, 0, sizeof(*request));
	mr = mach_msg(&request->head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(*request), exc_port, 10000, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS)
	{
		// no exception came: let the thread go on its own
		mprotect(page, getpagesize(), PROT_READ | PROT_WRITE);
		pthread_join(thread, NULL);
		return 0;
	}

	mprotect(page, getpagesize(), PROT_READ | PROT_WRITE);
	memset(&reply, 0, sizeof(reply));
	reply.head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request->head.msgh_bits), 0);
	reply.head.msgh_remote_port = request->head.msgh_remote_port;
	reply.head.msgh_size = sizeof(reply);
	reply.head.msgh_id = request->head.msgh_id + 100;
	reply.ndr = NDR_record;
	reply.ret_code = KERN_SUCCESS;
	mach_msg(&reply.head, MACH_SEND_MSG, sizeof(reply), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

	pthread_join(thread, NULL);
	return request->head.msgh_id;
}

int main(void)
{
	const int protected_codes = EXCEPTION_IDENTITY_PROTECTED | MACH_EXCEPTION_CODES;
	thread_act_t self = mach_thread_self();
	exception_mask_t masks[EXC_TYPES_COUNT];
	mach_port_t ports[EXC_TYPES_COUNT];
	exception_behavior_t behaviors[EXC_TYPES_COUNT];
	thread_state_flavor_t flavors[EXC_TYPES_COUNT];
	mach_msg_type_number_t count;
	request_t request;
	mach_msg_id_t id;
	kern_return_t kr, kr2, kr3, kr4;
	char detail[128];

	signal(SIGALRM, hung);
	alarm(30);

	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exc_port);
	mach_port_insert_right(mach_task_self(), exc_port, exc_port, MACH_MSG_TYPE_MAKE_SEND);
	page = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

	// without MACH_EXCEPTION_CODES: refused by each
	kr = thread_set_exception_ports(self, EXC_MASK_SOFTWARE, exc_port, EXCEPTION_IDENTITY_PROTECTED, THREAD_STATE_NONE);
	kr2 = task_set_exception_ports(mach_task_self(), EXC_MASK_SOFTWARE, exc_port, EXCEPTION_IDENTITY_PROTECTED,
		THREAD_STATE_NONE);
	count = EXC_TYPES_COUNT;
	kr3 = thread_swap_exception_ports(self, EXC_MASK_SOFTWARE, exc_port, EXCEPTION_IDENTITY_PROTECTED,
		THREAD_STATE_NONE, masks, &count, ports, behaviors, flavors);
	count = EXC_TYPES_COUNT;
	kr4 = task_swap_exception_ports(mach_task_self(), EXC_MASK_SOFTWARE, exc_port, EXCEPTION_IDENTITY_PROTECTED,
		THREAD_STATE_NONE, masks, &count, ports, behaviors, flavors);
	snprintf(detail, sizeof(detail), "thread_set %d, task_set %d, thread_swap %d, task_swap %d", kr, kr2, kr3, kr4);
	check(kr == KERN_INVALID_ARGUMENT && kr2 == KERN_INVALID_ARGUMENT && kr3 == KERN_INVALID_ARGUMENT
		&& kr4 == KERN_INVALID_ARGUMENT, "EXCEPTION_IDENTITY_PROTECTED without MACH_EXCEPTION_CODES is refused", detail);

	// Regression: refused with MACH_EXCEPTION_CODES too (searchd's "(os/kern) invalid argument")
	kr = thread_set_exception_ports(self, EXC_MASK_SOFTWARE, exc_port, protected_codes, THREAD_STATE_NONE);
	count = EXC_TYPES_COUNT;
	kr3 = thread_swap_exception_ports(self, EXC_MASK_SOFTWARE, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE,
		masks, &count, ports, behaviors, flavors);
	snprintf(detail, sizeof(detail), "thread_set %d, then swap %d, which found behavior %#x", kr, kr3,
		count == 1 ? behaviors[0] : 0);
	check(kr == KERN_SUCCESS && kr3 == KERN_SUCCESS && count == 1 && behaviors[0] == protected_codes,
		"a thread takes EXCEPTION_IDENTITY_PROTECTED with MACH_EXCEPTION_CODES", detail);
	if (count == 1 && MACH_PORT_VALID(ports[0]))
		mach_port_deallocate(mach_task_self(), ports[0]);

	count = EXC_TYPES_COUNT;
	kr2 = task_swap_exception_ports(mach_task_self(), EXC_MASK_SOFTWARE, exc_port, protected_codes, THREAD_STATE_NONE,
		masks, &count, ports, behaviors, flavors);
	// the task's old ones go back
	task_set_exception_ports(mach_task_self(), EXC_MASK_SOFTWARE, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
	for (mach_msg_type_number_t i = 0; kr2 == KERN_SUCCESS && i < count; i++)
	{
		if (!MACH_PORT_VALID(ports[i]))
			continue;
		task_set_exception_ports(mach_task_self(), masks[i], ports[i], behaviors[i], flavors[i]);
		mach_port_deallocate(mach_task_self(), ports[i]);
	}
	snprintf(detail, sizeof(detail), "task_swap %d", kr2);
	check(kr2 == KERN_SUCCESS, "a task takes it too", detail);

	// delivery itself, as a control
	id = fault_and_answer(EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, &request);
	snprintf(detail, sizeof(detail), "message ID %d, the thread %s", id, went_on ? "went on" : "didn't go on");
	check(id == MSGID_RAISE && request.raise.exception == EXC_BAD_ACCESS && went_on,
		"a fault comes to an EXCEPTION_DEFAULT port, and the thread goes on when it's answered", detail);
	if (id == MSGID_RAISE)
	{
		mach_port_deallocate(mach_task_self(), request.raise.thread.name);
		mach_port_deallocate(mach_task_self(), request.raise.task.name);
	}

	id = fault_and_answer(protected_codes, &request);
	snprintf(detail, sizeof(detail), "message ID %d, the thread %s", id, went_on ? "went on" : "didn't go on");
	check(id == MSGID_RAISE_IDENTITY_PROTECTED && request.protected_raise.exception == EXC_BAD_ACCESS
		&& request.protected_raise.code_count == 2 && request.protected_raise.code[1] == (int64_t) (uintptr_t) page
		&& went_on, "a fault comes to an EXCEPTION_IDENTITY_PROTECTED port as identity_protected, with its address, "
		"and the thread goes on when it's answered", detail);
	if (id == MSGID_RAISE_IDENTITY_PROTECTED)
	{
		mach_port_t token = request.protected_raise.task_id_token.name, task = MACH_PORT_NULL;

		snprintf(detail, sizeof(detail), "%#llx, the thread's is %#llx",
			(unsigned long long) request.protected_raise.thread_id, (unsigned long long) faulting_thread_id);
		check(faulting_thread_id != 0 && request.protected_raise.thread_id == faulting_thread_id,
			"its thread ID is the faulting thread's", detail);

		kr = task_identity_token_get_task_port(token, TASK_FLAVOR_CONTROL, &task);
		snprintf(detail, sizeof(detail), "%d, task port %#x (mach_task_self() is %#x)", kr, task, mach_task_self());
		check(kr == KERN_SUCCESS && task == mach_task_self(), "its task identity token gives back the task's port",
			detail);
		if (MACH_PORT_VALID(task))
			mach_port_deallocate(mach_task_self(), task);
		mach_port_deallocate(mach_task_self(), token);
	}

	alarm(0);
	return failures;
}
