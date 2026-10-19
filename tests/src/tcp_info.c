// getsockopt(TCP_CONNECTION_INFO) and getsockopt(TCP_INFO) of a TCP socket answer, as XNU's tcp_ctloutput() does, and
// socket() of a domain there are no sockets for is EAFNOSUPPORT, as XNU's socreate_internal() answers. libnetwork asks
// for both of every connection (nw_socket_copy_info) and logs each failure with a backtrace: they were ENOTSUP, and
// socket(PF_SYSTEM) (netsrc) was EINVAL.
// It only relies on XNU's interface, so it runs on macOS too.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// private: XNU's struct tcp_info starts with the state, as one byte
#ifndef TCP_INFO
#define TCP_INFO 0x200
#endif

// written before it's read: what the socket buffers hold however small the host keeps them
#define BYTES 16384

static int failures;

// write(), not printf(): stdout may be a file, fully buffered, which _exit() drops
static void hung(int sig)
{
	static const char line[] = "not ok - the connection carried its data within 30 s\n";

	(void) sig;
	write(STDOUT_FILENO, line, sizeof(line) - 1);
	_exit(1);
}

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	fflush(stdout);
	if (!ok)
		failures++;
}

static int connection_info(int fd, struct tcp_connection_info* info)
{
	socklen_t len = sizeof(*info);
	memset(info, 0, sizeof(*info));
	if (getsockopt(fd, IPPROTO_TCP, TCP_CONNECTION_INFO, info, &len) != 0)
		return -1;
	return len;
}

int main(void)
{
	struct sockaddr_in addr = { .sin_len = sizeof(addr), .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t addrlen = sizeof(addr);
	struct tcp_connection_info info;
	static char buffer[BYTES];
	int listener, client, server, len, got = 0;
	unsigned char state = 0xff;
	socklen_t statelen = sizeof(state);
	ssize_t n;

	signal(SIGALRM, hung);
	alarm(30);

	listener = socket(AF_INET, SOCK_STREAM, 0);
	bind(listener, (struct sockaddr*) &addr, sizeof(addr));
	listen(listener, 1);
	getsockname(listener, (struct sockaddr*) &addr, &addrlen);

	len = connection_info(listener, &info);
	check(len == sizeof(info) && info.tcpi_state == TCPS_LISTEN, "TCP_CONNECTION_INFO of a listening socket");

	client = socket(AF_INET, SOCK_STREAM, 0);
	connect(client, (struct sockaddr*) &addr, sizeof(addr));
	server = accept(listener, NULL, NULL);

	// a connection that has carried data both ways
	write(client, buffer, BYTES);
	while (got < BYTES && (n = read(server, buffer, sizeof(buffer))) > 0)
		got += n;
	write(server, "x", 1);
	read(client, buffer, 1);

	len = connection_info(client, &info);
	check(len == sizeof(info) && info.tcpi_state == TCPS_ESTABLISHED, "TCP_CONNECTION_INFO of a connection");
	check(info.tcpi_maxseg > 0 && info.tcpi_snd_cwnd >= info.tcpi_maxseg, "it has a segment size and a window");
	check(info.tcpi_txbytes >= BYTES && info.tcpi_rxbytes >= 1 && info.tcpi_txpackets > 0 && info.tcpi_rxpackets > 0,
		"and counts what the connection carried");

	// as much as fits
	check(getsockopt(client, IPPROTO_TCP, TCP_INFO, &state, &statelen) == 0 && statelen == 1
		&& state == TCPS_ESTABLISHED, "TCP_INFO, into a buffer of one byte: its state");

	alarm(0);
	close(client);
	close(server);
	close(listener);

	// AF_APPLETALK: XNU has no such domain
	check(socket(AF_APPLETALK, SOCK_DGRAM, 0) == -1 && errno == EAFNOSUPPORT, "socket() of a domain without sockets is EAFNOSUPPORT");

	return failures;
}
