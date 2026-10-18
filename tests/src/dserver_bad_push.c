// darlingserver survives a client that breaks its protocol, and answers it. A thread that got a reply it didn't
// expect while in a signal handler's interrupt_enter pushes it back (push_reply), for darlingserver to send again once
// the interrupt ends. darlingserver threw, and died with every process's calls, when a thread pushed one outside an
// interrupt or a second one for the same interrupt. A fork child that ran a signal handler on its parent's RPC socket
// made both ("Client-pushed reply overwriting existing saved reply"). Its other deaths from one client: a reply it
// couldn't send (ENOENT), an unknown call number, an interrupt_exit without an interrupt.
//
// Each case runs in a child, which sends the calls itself on a socket of its own, as the client's RPC wrappers would
// (its thread's replies then go to that socket), reports, and waits until this process kills it. darlingserver must
// answer what the case expects, and still answer this process afterwards. It's a white-box test: it mirrors the
// wrappers' framing (generate-rpc-wrappers.py: the header, the push's pipe as its last descriptor, replies that are
// only a header for a dropped call), and must change with them. Only a Darling test: it speaks darlingserver's
// protocol.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <darlingserver/rpc.h>
#include <errno.h>
#include <mach/mach.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define SERVER_SOCKET "/.darlingserver.sock"
#define WAIT_MS 5000
// a TID no thread has
#define NO_THREAD 0x3ffffff0
// darlingserver answers in Linux's errno numbers; ENOSYS is 78 on XNU (EINVAL and ESRCH are the same on both)
#define LINUX_ENOSYS 38

static int failures;

static void check(int ok, const char* what, const char* detail)
{
	if (ok)
		printf("ok - %s\n", what);
	else
	{
		printf("not ok - %s: %s\n", what, detail);
		failures++;
	}
}

static dserver_rpc_callhdr_t header(dserver_callnum_t number)
{
	dserver_rpc_callhdr_t h = {
		.number = number,
		.pid = getpid(),
		.tid = (pid_t) syscall(SYS_thread_selfid),
		.architecture = dserver_rpc_architecture_x86_64,
	};
	return h;
}

static void socket_path(char* path, size_t size)
{
	snprintf(path, size, "/private/var/tmp/dserver_bad_push.%d", getpid());
}

// a datagram socket to darlingserver, bound to this process's path (to get replies) if `bound`
static int server_socket(int bound)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int s = socket(AF_UNIX, SOCK_DGRAM, 0);

	if (s < 0)
		return -1;
	if (bound)
	{
		socket_path(addr.sun_path, sizeof(addr.sun_path));
		unlink(addr.sun_path);
		if (bind(s, (struct sockaddr*) &addr, sizeof(addr)) != 0)
			return -1;
	}
	strncpy(addr.sun_path, SERVER_SOCKET, sizeof(addr.sun_path) - 1);
	if (connect(s, (struct sockaddr*) &addr, sizeof(addr)) != 0)
		return -1;
	return s;
}

static int readable(int fd, int ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	return poll(&p, 1, ms) == 1;
}

// receives a reply within WAIT_MS into `reply`; returns its length, or -1
static ssize_t receive(int s, void* reply, size_t size)
{
	return readable(s, WAIT_MS) ? recv(s, reply, size, 0) : -1;
}

// pushes `reply` back as the client's wrappers do, from thread `tid`, with the pipe darlingserver writes to once it
// has taken (or dropped) the push, unless `pipe_too` is 0. Returns the byte it wrote, or -1 if none came in WAIT_MS.
static int push_reply_as(int s, pid_t tid, const void* reply, size_t size, int pipe_too)
{
	dserver_rpc_call_push_reply_t call = {
		.header = header(dserver_callnum_push_reply),
		.reply = (uint64_t) (uintptr_t) reply,
		.reply_size = size,
	};
	struct iovec iov = { .iov_base = &call, .iov_len = sizeof(call) };
	char control[CMSG_SPACE(sizeof(int))];
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	int pipe_ends[2];
	unsigned char byte;

	call.header.tid = tid;
	if (!pipe_too)
		return send(s, &call, sizeof(call), 0) == (ssize_t) sizeof(call) ? 0 : -1;
	if (pipe(pipe_ends) != 0)
		return -1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &pipe_ends[1], sizeof(int));
	if (sendmsg(s, &msg, 0) != (ssize_t) sizeof(call))
		return -1;
	close(pipe_ends[1]);
	// darlingserver writes a byte, then closes its end; if it dies, the read sees the end of the pipe
	int got = readable(pipe_ends[0], WAIT_MS) && read(pipe_ends[0], &byte, 1) == 1 ? byte : -1;
	close(pipe_ends[0]);
	return got;
}

static int push_reply(int s, const void* reply, size_t size)
{
	return push_reply_as(s, (pid_t) syscall(SYS_thread_selfid), reply, size, 1);
}

// sends a call of `size` bytes, its body zeroed, and returns its reply's code, or 1 if no reply came
static int call_sized(int s, dserver_callnum_t number, size_t size)
{
	union {
		dserver_rpc_callhdr_t header;
		char bytes[256];
	} c = { 0 };
	dserver_rpc_replyhdr_t reply;

	c.header = header(number);
	if (size > sizeof(c) || send(s, &c, size, 0) != (ssize_t) size)
		return 1;
	return receive(s, &reply, sizeof(reply)) >= (ssize_t) sizeof(reply) && reply.number == number ? reply.code : 1;
}

#define call(s, name) call_sized(s, dserver_callnum_ ## name, sizeof(dserver_rpc_call_ ## name ## _t))

// runs `body` in a child; returns what it wrote: '1' passed, anything else failed. The child waits to be killed,
// SETTLE_MS after it has reported, so that darlingserver has done with its calls by then.
#define SETTLE_MS 500
static char in_child(char (*body)(void))
{
	int result[2];
	char c = '?';

	if (pipe(result) != 0)
		return c;
	pid_t pid = fork();
	if (pid == 0)
	{
		c = body();
		write(result[1], &c, 1);
		for (;;)
			pause();
	}
	close(result[1]);
	// longer than any of the child's own waits, so that a hang reads as the child's answer
	if (!readable(result[0], 3 * WAIT_MS) || read(result[0], &c, 1) != 1)
		c = '?';
	close(result[0]);
	usleep(SETTLE_MS * 1000);
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	char path[64];
	snprintf(path, sizeof(path), "/private/var/tmp/dserver_bad_push.%d", pid);
	unlink(path);
	return c;
}

// a darlingserver call of this process's own
static int server_answers(void)
{
	mach_port_t self = mach_thread_self();
	return MACH_PORT_VALID(self) && mach_port_deallocate(mach_task_self(), self) == KERN_SUCCESS;
}

static const dserver_rpc_replyhdr_t first_body = { .number = (dserver_callnum_t) 0x51, .code = 1 };
static const dserver_rpc_replyhdr_t second_body = { .number = (dserver_callnum_t) 0x52, .code = 2 };

static char outside_interrupt(void)
{
	int s = server_socket(0);
	if (s < 0)
		return 's';
	int got = push_reply(s, &first_body, sizeof(first_body));
	return got == DSERVER_PUSH_REPLY_DROPPED ? '1' : got < 0 ? 'p' : 'b';
}

static char second_for_interrupt(void)
{
	dserver_rpc_replyhdr_t reply;
	int s = server_socket(1);

	if (s < 0)
		return 's';
	if (call(s, interrupt_enter) != 0)
		return 'i';
	if (push_reply(s, &first_body, sizeof(first_body)) != DSERVER_PUSH_REPLY_SAVED)
		return 'f';
	int got = push_reply(s, &second_body, sizeof(second_body));
	if (got != DSERVER_PUSH_REPLY_DROPPED)
		return got < 0 ? 'p' : 'b';
	// interrupt_exit's own reply, then the saved one
	if (call(s, interrupt_exit) != 0)
		return 'x';
	if (receive(s, &reply, sizeof(reply)) != (ssize_t) sizeof(reply))
		return 'n';
	return reply.number == first_body.number ? '1' : 'w';
}

// its socket's name unlinked before darlingserver replies to it; it lives until then
static char reply_to_gone_socket(void)
{
	char path[64];
	int s = server_socket(1);

	if (s < 0)
		return 's';
	socket_path(path, sizeof(path));
	unlink(path);
	// its reply can't arrive (nor any of this thread's now, so the child does nothing else)
	dserver_rpc_call_interrupt_enter_t c = { .header = header(dserver_callnum_interrupt_enter) };
	return send(s, &c, sizeof(c), 0) == (ssize_t) sizeof(c) ? '1' : 'c';
}

// a call number darlingserver doesn't know: answered with Linux's -ENOSYS
static char unknown_call(void)
{
	int s = server_socket(1);
	if (s < 0)
		return 's';
	int code = call_sized(s, (dserver_callnum_t) 0x7ffffff0, sizeof(dserver_rpc_callhdr_t));
	return code == -LINUX_ENOSYS ? '1' : code == 1 ? 'n' : 'e';
}

// a call too small for its number (kprintf has a body): answered with -EINVAL
static char too_small(void)
{
	int s = server_socket(1);
	if (s < 0)
		return 's';
	int code = call_sized(s, dserver_callnum_kprintf, sizeof(dserver_rpc_callhdr_t));
	return code == -EINVAL ? '1' : code == 1 ? 'n' : 'e';
}

// answered with -EINVAL
static char exit_without_enter(void)
{
	int s = server_socket(1);
	if (s < 0)
		return 's';
	int code = call(s, interrupt_exit);
	return code == -EINVAL ? '1' : code == 1 ? 'n' : 'e';
}

static char push_from_unknown_thread(void)
{
	int s = server_socket(0);
	if (s < 0)
		return 's';
	int got = push_reply_as(s, NO_THREAD, &first_body, sizeof(first_body), 1);
	return got == DSERVER_PUSH_REPLY_DROPPED ? '1' : got < 0 ? 'p' : 'b';
}

// its reply's address isn't mapped
static char push_unreadable(void)
{
	int s = server_socket(1);
	if (s < 0)
		return 's';
	if (call(s, interrupt_enter) != 0)
		return 'i';
	int got = push_reply(s, (const void*) 8, sizeof(first_body));
	return got == DSERVER_PUSH_REPLY_DROPPED ? '1' : got < 0 ? 'p' : 'b';
}

// nothing to wake it with: it just mustn't cost anyone else
static char push_without_pipe(void)
{
	int s = server_socket(0);
	if (s < 0)
		return 's';
	return push_reply_as(s, (pid_t) syscall(SYS_thread_selfid), &first_body, sizeof(first_body), 0) == 0 ? '1' : 'c';
}

static const struct {
	char (*body)(void);
	const char* what;
} cases[] = {
	{ outside_interrupt, "a reply pushed outside an interrupt is dropped, and its pusher woken" },
	{ second_for_interrupt, "of two replies pushed for one interrupt, the first is sent after it" },
	{ reply_to_gone_socket, "a live client's socket's name goes before its reply" },
	{ unknown_call, "an unknown call number is answered with ENOSYS" },
	{ too_small, "a call too small for its number is answered with EINVAL" },
	{ exit_without_enter, "an interrupt_exit without an interrupt is answered with EINVAL" },
	{ push_from_unknown_thread, "a push from a thread darlingserver doesn't know is dropped, and its pusher woken" },
	{ push_unreadable, "a push whose reply can't be read is dropped, and its pusher woken" },
	{ push_without_pipe, "a push without its pipe" },
};

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		char c = in_child(cases[i].body);
		const char* detail =
			c == 's' ? "no socket to darlingserver" : c == 'c' ? "the call wasn't sent" :
			c == 'p' ? "no answer to the push" : c == 'b' ? "the push wasn't dropped" :
			c == 'f' ? "the first push wasn't saved" : c == 'i' ? "interrupt_enter wasn't answered" :
			c == 'x' ? "interrupt_exit wasn't answered" : c == 'n' ? "no reply" :
			c == 'w' ? "the second push's reply was sent" : c == 'e' ? "the reply had another code" :
			"the child didn't report";
		check(c == '1', cases[i].what, detail);
		check(server_answers(), "darlingserver answers after it", "no");
	}
	return failures;
}
