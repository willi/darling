// darlingserver survives a client that pushes back a reply its thread's state can't take. A thread that got a
// reply it didn't expect while in a signal handler's interrupt_enter pushes it back (push_reply), for darlingserver
// to send again once the interrupt ends. darlingserver threw, and died with every process's calls, when a thread
// pushed one outside an interrupt or a second one for the same interrupt. A fork child that ran a signal handler on
// its parent's RPC socket made both ("Client-pushed reply overwriting existing saved reply").
//
// Each case runs in a child, which sends the calls itself on a socket of its own, as the client's RPC wrappers would,
// then kills itself (its thread's replies now go to that socket). darlingserver must answer each push, so that the
// child goes on, and still answer this process afterwards. A last child unlinks its socket's name before darlingserver
// replies to it: sending the reply failed with ENOENT, which darlingserver also died of. Another sends a call number
// darlingserver doesn't know: any call it throws for must cost only that call. Only a Darling test: it speaks
// darlingserver's protocol.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <darlingserver/rpc.h>
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

// a datagram socket to darlingserver; bound to `path` (to get replies) unless it's NULL
static int server_socket(const char* path)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int s = socket(AF_UNIX, SOCK_DGRAM, 0);

	if (s < 0)
		return -1;
	if (path)
	{
		strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
		unlink(path);
		if (bind(s, (struct sockaddr*) &addr, sizeof(addr)) != 0)
			return -1;
	}
	strncpy(addr.sun_path, SERVER_SOCKET, sizeof(addr.sun_path) - 1);
	if (connect(s, (struct sockaddr*) &addr, sizeof(addr)) != 0)
		return -1;
	return s;
}

static int readable(int fd)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	return poll(&p, 1, WAIT_MS) == 1;
}

// pushes a reply as the client's wrappers do, with the pipe darlingserver writes to once it has taken the push;
// returns whether it did within WAIT_MS
static int push_reply(int s)
{
	static dserver_rpc_replyhdr_t reply;
	dserver_rpc_call_push_reply_t call = {
		.header = header(dserver_callnum_push_reply),
		.reply = (uint64_t) (uintptr_t) &reply,
		.reply_size = sizeof(reply),
	};
	struct iovec iov = { .iov_base = &call, .iov_len = sizeof(call) };
	char control[CMSG_SPACE(sizeof(int))];
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control,
		.msg_controllen = sizeof(control),
	};
	struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
	int pipe_ends[2];
	char byte;

	if (pipe(pipe_ends) != 0)
		return 0;
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &pipe_ends[1], sizeof(int));
	if (sendmsg(s, &msg, 0) != (ssize_t) sizeof(call))
		return 0;
	close(pipe_ends[1]);
	// darlingserver writes a byte, then closes its end; if it dies, the read sees the end of the pipe
	int ok = readable(pipe_ends[0]) && read(pipe_ends[0], &byte, 1) == 1;
	close(pipe_ends[0]);
	return ok;
}

// sends interrupt_enter and waits for its reply
static int interrupt_enter(int s)
{
	dserver_rpc_call_interrupt_enter_t call = { .header = header(dserver_callnum_interrupt_enter) };
	dserver_rpc_reply_interrupt_enter_t reply;

	if (send(s, &call, sizeof(call), 0) != (ssize_t) sizeof(call))
		return 0;
	return readable(s) && recv(s, &reply, sizeof(reply), 0) == (ssize_t) sizeof(reply) && reply.header.code == 0;
}

// runs `body` in a child, which exits through SIGKILL; returns what it wrote: '1' passed, anything else failed
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
		kill(getpid(), SIGKILL);
	}
	close(result[1]);
	if (!readable(result[0]) || read(result[0], &c, 1) != 1)
		c = '?';
	close(result[0]);
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	return c;
}

static char outside_interrupt(void)
{
	int s = server_socket(NULL);
	if (s < 0)
		return 's';
	return push_reply(s) ? '1' : 'p';
}

static char second_for_interrupt(void)
{
	char path[64];
	snprintf(path, sizeof(path), "/private/var/tmp/dserver_bad_push.%d", getpid());
	int s = server_socket(path);
	if (s < 0)
		return 's';
	char c = !interrupt_enter(s) ? 'i' : !push_reply(s) ? 'f' : push_reply(s) ? '1' : 'p';
	unlink(path);
	return c;
}

// its socket's name unlinked before darlingserver replies to it
static char reply_to_gone_socket(void)
{
	char path[64];
	snprintf(path, sizeof(path), "/private/var/tmp/dserver_bad_push.%d", getpid());
	int s = server_socket(path);
	if (s < 0)
		return 's';
	unlink(path);
	// its reply can't arrive (nor any of this thread's now, so the child does nothing else)
	dserver_rpc_call_interrupt_enter_t call = { .header = header(dserver_callnum_interrupt_enter) };
	return send(s, &call, sizeof(call), 0) == (ssize_t) sizeof(call) ? '1' : 'c';
}

// a call number darlingserver doesn't know, which it threw for
static char unknown_call(void)
{
	int s = server_socket(NULL);
	if (s < 0)
		return 's';
	dserver_rpc_callhdr_t call = header((dserver_callnum_t) 0x7ffffff0);
	return send(s, &call, sizeof(call), 0) == (ssize_t) sizeof(call) ? '1' : 'c';
}

// a darlingserver call of this process's own, answered or not
static int server_answers(void)
{
	mach_port_t self = mach_thread_self();
	return MACH_PORT_VALID(self) && mach_port_deallocate(mach_task_self(), self) == KERN_SUCCESS;
}

int main(void)
{
	char c;

	setvbuf(stdout, NULL, _IOLBF, 0);

	c = in_child(outside_interrupt);
	check(c == '1', "a reply pushed outside an interrupt is answered",
		c == 'p' ? "no answer" : c == 's' ? "no socket to darlingserver" : "the child didn't report");
	check(server_answers(), "darlingserver answers after it", "no");

	c = in_child(second_for_interrupt);
	check(c == '1', "a second reply pushed for one interrupt is answered",
		c == 'p' ? "no answer" : c == 'f' ? "the first push wasn't answered" :
		c == 'i' ? "interrupt_enter wasn't answered" : c == 's' ? "no socket to darlingserver" :
		"the child didn't report");
	check(server_answers(), "darlingserver answers after it", "no");

	c = in_child(reply_to_gone_socket);
	check(c == '1', "a client's socket's name goes before its reply", c == 's' ? "no socket to darlingserver" :
		c == 'c' ? "the call wasn't sent" : "the child didn't report");
	// time for darlingserver to try the reply
	usleep(500000);
	check(server_answers(), "darlingserver answers after it", "no");

	c = in_child(unknown_call);
	check(c == '1', "a client sends a call number darlingserver doesn't know", c == 's' ? "no socket to darlingserver" :
		c == 'c' ? "the call wasn't sent" : "the child didn't report");
	usleep(500000);
	check(server_answers(), "darlingserver answers after it", "no");

	return failures;
}
