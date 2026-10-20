// `darling shell` and `darling PROGRAM` exit with what shellspawn reports for the program it ran. A program a signal
// killed has to come back as a shell reports it, 128 + the signal: shellspawn sent WEXITSTATUS alone, so a crash read as
// exit 0, and a harness that took `darling shell`'s exit status as the verdict passed it. Talks to the prefix's
// shellspawn as the darling client does, both ways it runs something: a login shell's `-c`, and an executable
// (SHELLSPAWN_SETEXEC).
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../src/shellspawn/shellspawn.h"

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	fflush(stdout);
	if (!ok)
		failures++;
}

static int push(int fd, shellspawn_cmd_type_t type, const char* value)
{
	size_t n = value ? strlen(value) + 1 : 0;
	char buf[sizeof(struct shellspawn_cmd) + 256];
	struct shellspawn_cmd* cmd = (struct shellspawn_cmd*) buf;
	if (n > 256)
		return -1;
	cmd->cmd = type;
	cmd->data_length = n;
	if (n)
		memcpy(cmd->data, value, n);
	return write(fd, buf, sizeof(*cmd) + n) == (ssize_t) (sizeof(*cmd) + n) ? 0 : -1;
}

static int go(int fd, int io)
{
	struct shellspawn_cmd cmd = { SHELLSPAWN_GO, 0 };
	int fds[3] = { io, io, io };
	char cbuf[CMSG_SPACE(sizeof(fds))];
	struct iovec iov = { &cmd, sizeof(cmd) };
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);
	struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
	c->cmsg_len = CMSG_LEN(sizeof(fds));
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	memcpy(CMSG_DATA(c), fds, sizeof(fds));
	return sendmsg(fd, &msg, 0) < 0 ? -1 : 0;
}

// runs exec (a login shell when NULL) with args through shellspawn, and returns the exit status it reports (-1: the
// exchange failed)
static int run(const char* exec, const char* const args[])
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX, .sun_path = SHELLSPAWN_SOCKPATH };
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	int io = open("/dev/null", O_RDWR);
	int status = -1;
	if (fd < 0 || io < 0 || connect(fd, (struct sockaddr*) &addr, sizeof(addr)) != 0)
	{
		printf("# shellspawn at %s: %s\n", SHELLSPAWN_SOCKPATH, strerror(errno));
		goto out;
	}
	if (exec && push(fd, SHELLSPAWN_SETEXEC, exec) != 0)
		goto out;
	for (; *args; args++)
		if (push(fd, SHELLSPAWN_ADDARG, *args) != 0)
			goto out;
	if (go(fd, io) != 0 || read(fd, &status, sizeof(status)) != sizeof(status))
		status = -1;
out:
	if (fd >= 0)
		close(fd);
	if (io >= 0)
		close(io);
	return status;
}

int main(void)
{
	char what[128];
	int s;

	// a login shell's -c: the shell itself is the program
	s = run(NULL, (const char* const[]) { "-c", "exit 3", NULL });
	snprintf(what, sizeof(what), "a shell's exit 3 comes back as 3 (%d)", s);
	check(s == 3, what);

	s = run(NULL, (const char* const[]) { "-c", "kill -SEGV $$", NULL });
	snprintf(what, sizeof(what), "a shell SIGSEGV killed comes back as 128 + SIGSEGV (%d)", s);
	check(s == 128 + SIGSEGV, what);

	// an executable (`darling PROGRAM`)
	s = run("/bin/sh", (const char* const[]) { "sh", "-c", "kill -ILL $$", NULL });
	snprintf(what, sizeof(what), "an executable SIGILL killed comes back as 128 + SIGILL (%d)", s);
	check(s == 128 + SIGILL, what);

	return failures;
}
