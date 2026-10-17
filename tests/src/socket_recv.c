// Receiving on a socket as on XNU: accept() without the peer's address, and recv()'s flags. Darling dereferenced
// accept()'s address unconditionally (accept(fd, NULL, NULL) crashed), and passed recvfrom()'s flags to Linux
// unconverted, so MSG_DONTWAIT (BSD 0x80) reached Linux as MSG_EOR and blocked, and MSG_WAITALL (BSD 0x40) as
// MSG_DONTWAIT.
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

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
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

	close(s);
	close(c);
	close(lsn);
	return failures;
}
