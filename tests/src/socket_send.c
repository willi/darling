// Sending on a socket as on XNU: SIGPIPE, SO_NOSIGPIPE and MSG_NOSIGNAL, and the error of a send after a reset.
// Darling ignored SO_NOSIGPIPE (and MSG_NOSIGNAL), so a send to a closed connection killed programs that had asked
// for none: Network.framework sets SO_NOSIGPIPE on its sockets, and the iOS Simulator's networking processes died of
// SIGPIPE when a server closed a connection. And Linux fails the first send after a reset with ECONNRESET, where XNU
// fails it with EPIPE and keeps the error for recv() and SO_ERROR.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int failures;
static volatile int sigpipes;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

static void on_sigpipe(int sig)
{
	(void) sig;
	sigpipes++;
}

static int lsn;
static struct sockaddr_in addr;

// a connected client, with the server's end in *server
static int pair(int* server)
{
	int c = socket(AF_INET, SOCK_STREAM, 0);

	connect(c, (struct sockaddr*) &addr, sizeof(addr));
	*server = accept(lsn, NULL, NULL);
	return c;
}

// a connected client that can't send any more (XNU's SS_CANTSENDMORE): sends fail with EPIPE
static int closed_for_sending(void)
{
	int s, c = pair(&s);

	shutdown(c, SHUT_WR);
	close(s);
	return c;
}

struct sender {
	int fd;
	long result;
	int error;
};

static void* send_blocking_sigpipe(void* arg)
{
	struct sender* s = arg;
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	s->result = send(s->fd, "x", 1, 0);
	s->error = errno;
	return NULL;
}

static int nosigpipe(int fd)
{
	int on = -1;
	socklen_t len = sizeof(on);

	getsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, &len);
	return on;
}

int main(void)
{
	struct sigaction sa;
	socklen_t len = sizeof(addr);
	int one = 1, before;
	char b;

	setvbuf(stdout, NULL, _IOLBF, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigpipe;
	sigaction(SIGPIPE, &sa, NULL);

	lsn = socket(AF_INET, SOCK_STREAM, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(lsn, (struct sockaddr*) &addr, sizeof(addr));
	listen(lsn, 4);
	getsockname(lsn, (struct sockaddr*) &addr, &len);

	{
		int c = closed_for_sending();
		before = sigpipes;
		check(write(c, "x", 1) < 0 && errno == EPIPE && sigpipes == before + 1, "write() fails with EPIPE and raises SIGPIPE");
		before = sigpipes;
		check(send(c, "x", 1, 0) < 0 && errno == EPIPE && sigpipes == before + 1, "send() fails with EPIPE and raises SIGPIPE");
		before = sigpipes;
		check(send(c, "x", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE && sigpipes == before,
		    "send(MSG_NOSIGNAL) fails with EPIPE without SIGPIPE");
		check(nosigpipe(c) == 0, "SO_NOSIGPIPE is off by default");
		close(c);
	}

	{
		int c = closed_for_sending();
		struct iovec iov = { "x", 1 };
		struct msghdr msg;
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;

		check(setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == 0, "SO_NOSIGPIPE can be set");
		check(nosigpipe(c) == 1, "and reads back as set");
		before = sigpipes;
		check(write(c, "x", 1) < 0 && errno == EPIPE, "with SO_NOSIGPIPE, write() fails with EPIPE");
		check(writev(c, &iov, 1) < 0 && errno == EPIPE, "and writev()");
		check(send(c, "x", 1, 0) < 0 && errno == EPIPE, "and send()");
		check(sendmsg(c, &msg, 0) < 0 && errno == EPIPE, "and sendmsg()");
		check(sigpipes == before, "none of them raises SIGPIPE");

		// the flag is the socket's: a copy of the descriptor has it too
		int d = dup(c);
		check(nosigpipe(d) == 1, "a dup() of the descriptor has SO_NOSIGPIPE");
		check(write(d, "x", 1) < 0 && errno == EPIPE && sigpipes == before, "and writes to it raise no SIGPIPE");
		close(d);
		d = fcntl(c, F_DUPFD, 0);
		check(nosigpipe(d) == 1 && write(d, "x", 1) < 0 && sigpipes == before, "and an F_DUPFD copy");
		int e = socket(AF_INET, SOCK_STREAM, 0);
		check(dup2(c, e) == e && nosigpipe(e) == 1 && write(e, "x", 1) < 0 && sigpipes == before,
		    "and a dup2() copy over another socket");
		// a dup2() that fails replaces nothing (the flag of the descriptor it would have replaced was dropped)
		check(dup2(-1, d) < 0 && errno == EBADF, "dup2() from a bad descriptor fails");
		check(nosigpipe(d) == 1 && write(d, "x", 1) < 0 && sigpipes == before, "and the target keeps SO_NOSIGPIPE");
		close(d);
		close(e);

		int zero = 0;
		setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &zero, sizeof(zero));
		check(nosigpipe(c) == 0, "SO_NOSIGPIPE can be cleared");
		check(write(c, "x", 1) < 0 && errno == EPIPE && sigpipes == before + 1, "then write() raises SIGPIPE again");
		close(c);
	}

	{
		// a closed descriptor's number, reused by a socket without the flag, doesn't inherit it
		int c = closed_for_sending();
		setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
		close(c);
		int again = closed_for_sending();
		before = sigpipes;
		check(write(again, "x", 1) < 0 && errno == EPIPE && sigpipes == before + 1,
		    "a new socket on a reused descriptor number raises SIGPIPE");
		close(again);
	}

	{
		// the peer resets the connection: XNU's sosendcheck() fails the send with EPIPE (the socket was
		// disconnected), raising SIGPIPE, and so_error stays for recv()
		int s, c = pair(&s);
		struct linger lg = { 1, 0 };
		setsockopt(s, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
		close(s);
		struct pollfd pfd = { c, POLLIN, 0 };
		poll(&pfd, 1, 2000);
		before = sigpipes;
		check(send(c, "x", 1, 0) < 0 && errno == EPIPE, "the first send after a reset fails with EPIPE");
		check(sigpipes == before + 1, "and raises SIGPIPE");
		check(recv(c, &b, 1, 0) < 0 && errno == ECONNRESET, "recv() then fails with ECONNRESET");
		check(recv(c, &b, 1, 0) == 0, "and then finds end of file");
		close(c);
	}

	{
		// XNU's sendit() raises SIGPIPE for the process (psignal()): a sending thread that blocks it doesn't stop
		// another thread taking it (it was sent to the sending thread, where it stayed blocked)
		struct sender sender = { closed_for_sending(), 0 };
		pthread_t t;
		before = sigpipes;
		pthread_create(&t, NULL, send_blocking_sigpipe, &sender);
		pthread_join(t, NULL);
		for (int i = 0; i < 100 && sigpipes == before; i++)
			usleep(10000);
		check(sender.result < 0 && sender.error == EPIPE, "a thread blocking SIGPIPE gets EPIPE");
		check(sigpipes == before + 1, "and the process takes the SIGPIPE in another thread");
		close(sender.fd);
	}

	{
		// SO_NOSIGPIPE on something that isn't a socket
		int p[2];
		pipe(p);
		errno = 0;
		check(setsockopt(p[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0 && errno == ENOTSOCK,
		    "SO_NOSIGPIPE on a pipe fails with ENOTSOCK");
		// a pipe without a reader: Linux raises SIGPIPE itself, once
		close(p[0]);
		before = sigpipes;
		check(write(p[1], "x", 1) < 0 && errno == EPIPE && sigpipes == before + 1,
		    "a write to a pipe without a reader raises SIGPIPE once");
		close(p[1]);
	}

	close(lsn);
	return failures;
}
