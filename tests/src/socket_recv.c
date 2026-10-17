// Receiving on a socket as on XNU: accept() without the peer's address, recv()'s flags, and SO_NREAD. Darling
// dereferenced accept()'s address unconditionally (accept(fd, NULL, NULL) crashed); passed recvfrom()'s flags to Linux
// unconverted, so MSG_DONTWAIT (BSD 0x80) reached Linux as MSG_EOR and blocked, and MSG_WAITALL (BSD 0x40) as
// MSG_DONTWAIT; and failed SO_NREAD with EINVAL, which Network.framework asks before each read.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef SO_NREAD
#define SO_NREAD 0x1020
#endif

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

static int nread(int fd)
{
	int n = -1;
	socklen_t len = sizeof(n);

	return getsockopt(fd, SOL_SOCKET, SO_NREAD, &n, &len) == 0 && len == sizeof(n) ? n : -1;
}

static void on_alarm(int sig)
{
	(void) sig;
}

static void* send_later(void* arg)
{
	usleep(200000);
	send(*(int*) arg, "b", 1, 0);
	return NULL;
}

int main(void)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	struct sigaction sa;
	char buf[4];

	setvbuf(stdout, NULL, _IOLBF, 0);

	// a blocked recv() fails with EINTR at the alarm instead of hanging the test
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alarm;
	sigaction(SIGALRM, &sa, NULL);

	int lsn = socket(AF_INET, SOCK_STREAM, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(lsn, (struct sockaddr*) &addr, sizeof(addr));
	listen(lsn, 1);
	getsockname(lsn, (struct sockaddr*) &addr, &len);

	int c = socket(AF_INET, SOCK_STREAM, 0);
	connect(c, (struct sockaddr*) &addr, sizeof(addr));
	int s = accept(lsn, NULL, NULL);
	check(s >= 0, "accept() without the peer's address");

	alarm(2);
	errno = 0;
	check(recv(c, buf, 1, MSG_DONTWAIT) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
	    "recv(MSG_DONTWAIT) on a blocking socket without data fails with EAGAIN");
	alarm(0);

	pthread_t t;
	send(s, "a", 1, 0);
	pthread_create(&t, NULL, send_later, &s);
	alarm(2);
	check(recv(c, buf, 2, MSG_WAITALL) == 2, "recv(MSG_WAITALL) waits for all the bytes");
	alarm(0);
	pthread_join(t, NULL);

	// SO_NREAD: a stream socket's bytes to read; a listening socket has none
	send(s, "abc", 3, 0);
	usleep(100000);
	check(nread(c) == 3, "SO_NREAD on a stream socket is the bytes to read");
	check(nread(lsn) == 0, "SO_NREAD on a listening socket is 0");
	recv(c, buf, 3, 0);
	check(nread(c) == 0, "and 0 once they're read");

	// a datagram socket's: the first datagram's size
	int u = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in uaddr;
	socklen_t ulen = sizeof(uaddr);
	memset(&uaddr, 0, sizeof(uaddr));
	uaddr.sin_family = AF_INET;
	uaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(u, (struct sockaddr*) &uaddr, sizeof(uaddr));
	getsockname(u, (struct sockaddr*) &uaddr, &ulen);
	int v = socket(AF_INET, SOCK_DGRAM, 0);
	sendto(v, "ab", 2, 0, (struct sockaddr*) &uaddr, sizeof(uaddr));
	sendto(v, "cdefg", 5, 0, (struct sockaddr*) &uaddr, sizeof(uaddr));
	usleep(100000);
	check(nread(u) == 2, "SO_NREAD on a datagram socket is the first datagram's size");
	int p[2];
	pipe(p);
	errno = 0;
	int n;
	socklen_t nlen = sizeof(n);
	check(getsockopt(p[0], SOL_SOCKET, SO_NREAD, &n, &nlen) < 0 && errno == ENOTSOCK, "SO_NREAD on a pipe fails with ENOTSOCK");
	close(p[0]);
	close(p[1]);
	close(u);
	close(v);

	close(s);
	close(c);
	close(lsn);
	return failures;
}
