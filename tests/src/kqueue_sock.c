// EVFILT_SOCK as XNU has it (bsd/kern/uipc_socket.c, filt_sockev_common): the iOS Simulator's Network.framework
// learns that a nonblocking connect() completed, failed or was reset from it. Darling had no EVFILT_SOCK (kevent()
// failed the registration with ENOSYS), so every TCP connection Network.framework opened stayed "in progress" until
// its timeout.
//
// It only relies on XNU's interface, so it runs on macOS too.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// XNU's private EVFILT_SOCK interface (sys/event.h and sys/socketvar.h, PRIVATE)
#ifndef EVFILT_SOCK
#define EVFILT_SOCK (-13)
#endif
#ifndef NOTE_CONNRESET
#define NOTE_CONNRESET 0x00000001
#define NOTE_READCLOSED 0x00000002
#define NOTE_WRITECLOSED 0x00000004
#define NOTE_TIMEOUT 0x00000008
#define NOTE_SUSPEND 0x00000040
#define NOTE_RESUME 0x00000080
#define NOTE_CONNECTED 0x00000800
#define NOTE_DISCONNECTED 0x00001000
#endif
#ifndef SOCKEV_CONNECTED
#define SOCKEV_CONNECTED 0x00000001
#endif

#define STATE_NOTES (NOTE_CONNRESET | NOTE_READCLOSED | NOTE_WRITECLOSED | NOTE_TIMEOUT | NOTE_CONNECTED | NOTE_DISCONNECTED)

// more sockets than libsystem_kernel remembers at once (sostate.h's SOCKET_ENTRIES_MAX)
#define SOCKETS_WITH_ERRORS 140

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

static int listener(struct sockaddr_in* addr)
{
	socklen_t len = sizeof(*addr);
	int s = socket(AF_INET, SOCK_STREAM, 0);

	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(s, (struct sockaddr*) addr, sizeof(*addr)) < 0 || listen(s, 4) < 0 ||
	    getsockname(s, (struct sockaddr*) addr, &len) < 0)
		return -1;
	return s;
}

// a nonblocking socket connecting to addr; *err is connect()'s errno (EINPROGRESS, or 0 if it connected at once)
static int connecting(const struct sockaddr_in* addr, int* err)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);

	fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
	*err = connect(s, (const struct sockaddr*) addr, sizeof(*addr)) < 0 ? errno : 0;
	return s;
}

// registers EVFILT_SOCK for fd, as libdispatch's DISPATCH_SOURCE_TYPE_SOCK does (EV_CLEAR); the registration's errno
static long add(int kq, int fd, unsigned fflags)
{
	struct kevent kev, out;

	EV_SET(&kev, fd, EVFILT_SOCK, EV_ADD | EV_CLEAR | EV_RECEIPT, fflags, 0, NULL);
	if (kevent(kq, &kev, 1, &out, 1, NULL) != 1)
		return errno;
	return (out.flags & EV_ERROR) ? (long) out.data : -1;
}

// the next EVFILT_SOCK event for fd within ms milliseconds; 0 if none came
static int next(int kq, int fd, long ms, struct kevent* out)
{
	struct timespec timeout = { ms / 1000, (ms % 1000) * 1000000 };

	while (kevent(kq, NULL, 0, out, 1, &timeout) == 1) {
		if (out->filter == EVFILT_SOCK && out->ident == (uintptr_t) fd)
			return 1;
	}
	return 0;
}

// the next event for fd whose fflags include note
static int next_with(int kq, int fd, unsigned note, struct kevent* out)
{
	for (int i = 0; i < 10; i++) {
		if (!next(kq, fd, 2000, out))
			return 0;
		if (out->fflags & note)
			return 1;
	}
	return 0;
}

static int so_error(int fd)
{
	int err = -1;
	socklen_t len = sizeof(err);

	getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
	return err;
}

// a connection from a nonblocking client with an EVFILT_SOCK knote, once NOTE_CONNECTED came; 0 if setting it up failed
static int connected_pair(int lsn, const struct sockaddr_in* addr, int kq, int* client, int* server)
{
	struct kevent ev;
	int err;

	*client = connecting(addr, &err);
	if (add(kq, *client, STATE_NOTES) != 0 || !next_with(kq, *client, NOTE_CONNECTED, &ev))
		return 0;
	*server = accept(lsn, NULL, NULL);
	return *server >= 0;
}

// how many events for fd one kevent() call returns
static int events_for(int kq, int fd)
{
	struct kevent out[8];
	struct timespec timeout = { 1, 0 };
	int n = kevent(kq, NULL, 0, out, 8, &timeout), count = 0;

	for (int i = 0; i < n; i++)
		count += out[i].ident == (uintptr_t) fd;
	return count;
}

static long change(int kq, int fd, short filter, unsigned short flags, unsigned fflags)
{
	struct kevent kev, out;

	EV_SET(&kev, fd, filter, flags | EV_RECEIPT, fflags, 0, NULL);
	if (kevent(kq, &kev, 1, &out, 1, NULL) != 1)
		return errno;
	return (out.flags & EV_ERROR) ? (long) out.data : -1;
}

// the peer resets the connection
static void reset(int s)
{
	struct linger lg = { 1, 0 };

	setsockopt(s, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
	close(s);
}

static void on_alarm(int sig)
{
	(void) sig;
}

// a blocking connection without a knote; -1 if setting it up failed
static int tcp_pair(int lsn, const struct sockaddr_in* addr, int* server)
{
	int c = socket(AF_INET, SOCK_STREAM, 0);

	if (connect(c, (const struct sockaddr*) addr, sizeof(*addr)) < 0)
		return -1;
	*server = accept(lsn, NULL, NULL);
	return *server >= 0 ? c : -1;
}

// waits up to 2 seconds for poll() to report bits on fd
static int poll_for(int fd, short bits)
{
	for (int i = 0; i < 200; i++) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		if (poll(&pfd, 1, 10) > 0 && (pfd.revents & bits) == bits)
			return 1;
		usleep(10000);
	}
	return 0;
}

// a reset after the peer's FIN: c's connection is in CLOSE_WAIT, then reset; returns once c is closed (hung up)
static int reset_after_fin(int c, int s)
{
	shutdown(s, SHUT_WR);
	if (!poll_for(c, POLLIN))
		return 0;
	reset(s);
	return poll_for(c, POLLHUP);
}

// a nonblocking connect to closed that was refused, once it was
static int refused(const struct sockaddr_in* closed)
{
	int err, c = connecting(closed, &err);
	struct pollfd pfd = { c, POLLOUT, 0 };

	poll(&pfd, 1, 2000);
	return c;
}

int main(void)
{
	struct sockaddr_in addr, closed;
	struct kevent ev;
	int kq = kqueue();
	int lsn = listener(&addr);
	int err;

	setvbuf(stdout, NULL, _IOLBF, 0);

	{
		// XNU's soisconnected() posts SO_FILT_HINT_CONNECTED; Network.framework waits for it after EINPROGRESS
		int c = connecting(&addr, &err);
		long r = add(kq, c, STATE_NOTES);
		check(r == 0, "EVFILT_SOCK registers on a socket");
		int got = next_with(kq, c, NOTE_CONNECTED, &ev);
		check(got, "a nonblocking connect fires NOTE_CONNECTED");
		check(got && (ev.data & SOCKEV_CONNECTED) && !(ev.flags & EV_EOF),
		    "NOTE_CONNECTED's data is SOCKEV_CONNECTED, without EV_EOF");
		check(!next(kq, c, 300, &ev), "a level that was delivered isn't delivered again");

		// the peer's FIN: SS_CANTRCVMORE
		int s = accept(lsn, NULL, NULL);
		shutdown(s, SHUT_WR);
		got = next_with(kq, c, NOTE_READCLOSED, &ev);
		check(got && !(ev.fflags & NOTE_WRITECLOSED) && !(ev.flags & EV_EOF),
		    "the peer's shutdown fires NOTE_READCLOSED alone, without EV_EOF");

		// our own shutdown: SS_CANTSENDMORE
		shutdown(c, SHUT_WR);
		check(next_with(kq, c, NOTE_WRITECLOSED, &ev), "shutdown(SHUT_WR) fires NOTE_WRITECLOSED");
		close(s);
		close(c);
	}

	{
		// a socket that hasn't connected is neither connected nor disconnected (Linux reports it hung up)
		int c = socket(AF_INET, SOCK_STREAM, 0);
		add(kq, c, STATE_NOTES);
		check(!next(kq, c, 300, &ev), "a socket that never connected fires nothing");
		close(c);
	}

	{
		// a port nothing listens on: XNU's SYN_SENT RST posts SO_FILT_HINT_CONNRESET and drops with ECONNREFUSED.
		// The knote is attached before connect(), so it sees the reset (XNU's hints reach only attached knotes).
		int t = listener(&closed);
		close(t);
		int c = socket(AF_INET, SOCK_STREAM, 0);
		fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
		add(kq, c, STATE_NOTES);
		connect(c, (struct sockaddr*) &closed, sizeof(closed));
		int got = next_with(kq, c, NOTE_CONNRESET, &ev);
		check(got && (ev.flags & EV_EOF) && ev.data == ECONNREFUSED,
		    "a refused connect fires NOTE_CONNRESET and EV_EOF with ECONNREFUSED");
		check(got && !(ev.fflags & NOTE_CONNECTED), "a refused connect doesn't fire NOTE_CONNECTED");
		// XNU's filter reads so_error without clearing it; the program still gets it
		check(so_error(c) == ECONNREFUSED, "SO_ERROR is still ECONNREFUSED after the event");
		check(so_error(c) == 0, "reading SO_ERROR clears it");
		close(c);
	}

	{
		// the peer's RST on an established connection
		int c, s;
		check(connected_pair(lsn, &addr, kq, &c, &s), "set up a connection");
		reset(s);
		int got = next_with(kq, c, NOTE_CONNRESET, &ev);
		check(got && (ev.flags & EV_EOF) && ev.data == ECONNRESET,
		    "the peer's reset fires NOTE_CONNRESET and EV_EOF with ECONNRESET");
		check(got && (ev.fflags & NOTE_READCLOSED) && (ev.fflags & NOTE_WRITECLOSED),
		    "after a reset both directions are closed");
		// XNU's soreceive() returns the pending error before end of file
		char b;
		check(recv(c, &b, 1, 0) < 0 && errno == ECONNRESET, "recv() after the reset fails with ECONNRESET");
		check(so_error(c) == 0, "recv() took the error");
		close(c);
	}

	{
		// the fflags a knote was registered with filter what it reports
		int c = connecting(&addr, &err);
		add(kq, c, NOTE_READCLOSED);
		check(!next(kq, c, 300, &ev), "NOTE_CONNECTED isn't reported when not asked for");
		int s = accept(lsn, NULL, NULL);
		close(s);
		check(next(kq, c, 2000, &ev) && ev.fflags == NOTE_READCLOSED, "only the notes asked for are reported");
		close(c);
	}

	{
		// registering on a socket whose connect was already refused: the error is pending when the knote attaches,
		// and Linux reports the socket ready as well, but one kevent() reports the knote once (it was reported twice,
		// and with EV_DISPATCH the second report disabled the knote again: an assertion failure)
		int t = listener(&closed);
		close(t);
		int c = connecting(&closed, &err);
		struct pollfd pfd = { c, POLLOUT, 0 };
		poll(&pfd, 1, 2000);
		check(change(kq, c, EVFILT_SOCK, EV_ADD | EV_CLEAR | EV_DISPATCH, STATE_NOTES) == 0,
		    "EVFILT_SOCK registers with EV_DISPATCH on a refused socket");
		check(events_for(kq, c) == 1, "a knote whose error is pending is reported once per kevent()");
		// XNU's EV_ENABLE re-arms a dispatched knote, and enabling an enabled one is no error
		check(change(kq, c, EVFILT_SOCK, EV_ENABLE, STATE_NOTES) == 0, "EV_ENABLE re-arms it");
		check(change(kq, c, EVFILT_SOCK, EV_ENABLE, STATE_NOTES) == 0, "EV_ENABLE on an enabled knote succeeds");
		check(next(kq, c, 2000, &ev) && (ev.flags & EV_EOF) && ev.data == ECONNREFUSED,
		    "re-armed, it reports the pending error again");
		close(c);
	}

	{
		// without EV_CLEAR, a level-triggered note is delivered once, as filt_sockev_common() does
		int c = connecting(&addr, &err);
		struct kevent kev;
		EV_SET(&kev, c, EVFILT_SOCK, EV_ADD, STATE_NOTES, 0, NULL);
		kevent(kq, &kev, 1, NULL, 0, NULL);
		check(next_with(kq, c, NOTE_CONNECTED, &ev), "without EV_CLEAR, NOTE_CONNECTED is delivered");
		// the knote looks again, finds nothing new and reports nothing: kevent() still waits out its timeout
		// (it returned 0 at once when every event it found was dropped)
		struct timespec t0, t1, wait = { 0, 300000000 };
		clock_gettime(CLOCK_MONOTONIC, &t0);
		int n = kevent(kq, NULL, 0, &ev, 1, &wait);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		long waited = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
		check(n == 0, "and only once");
		check(waited >= 250, "kevent() waits out its timeout though the knote woke with nothing to report");
		close(accept(lsn, NULL, NULL));
		check(next_with(kq, c, NOTE_READCLOSED, &ev) && (ev.fflags & NOTE_CONNECTED),
		    "a later event still carries NOTE_CONNECTED, without EV_CLEAR");
		close(c);
	}

	{
		// the peer's reset after its FIN, in CLOSE_WAIT: Linux's error there is EPIPE, XNU's ECONNRESET
		int c, s;
		char b;
		check(connected_pair(lsn, &addr, kq, &c, &s), "set up a connection");
		shutdown(s, SHUT_WR);
		check(next_with(kq, c, NOTE_READCLOSED, &ev), "the peer's FIN fires NOTE_READCLOSED");
		reset(s);
		int got = next_with(kq, c, NOTE_CONNRESET, &ev);
		check(got && (ev.flags & EV_EOF) && ev.data == ECONNRESET,
		    "a reset in CLOSE_WAIT fires NOTE_CONNRESET with ECONNRESET");
		check(recv(c, &b, 1, 0) < 0 && errno == ECONNRESET, "recv() after it fails with ECONNRESET");
		close(c);
	}

	{
		// the same reset, with nothing looking first: Linux's recv() returns end of file before the error (it
		// returned 0 here), XNU's soreceive() the error
		int s, c = tcp_pair(lsn, &addr, &s);
		char b;
		check(c >= 0 && reset_after_fin(c, s), "a reset after the peer's FIN, on a socket without a knote");
		check(recv(c, &b, 1, 0) < 0 && errno == ECONNRESET, "recv() fails with ECONNRESET before end of file");
		check(recv(c, &b, 1, 0) == 0 && so_error(c) == 0, "then finds end of file, the error taken");
		close(c);
	}

	{
		// and with a send first, on a watched socket: Linux's send() returns EPIPE and clears the error, as it
		// does on a socket shut down for sending (the error was lost: recv() found end of file, the knote no reset)
		int c, s;
		char b;
		check(connected_pair(lsn, &addr, kq, &c, &s) && reset_after_fin(c, s),
		    "a reset after the peer's FIN, on a watched socket");
		check(send(c, "x", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE, "a send fails with EPIPE");
		int got = next_with(kq, c, NOTE_CONNRESET, &ev);
		check(got && (ev.flags & EV_EOF) && ev.data == ECONNRESET,
		    "the knote still fires NOTE_CONNRESET with ECONNRESET");
		check(recv(c, &b, 1, 0) < 0 && errno == ECONNRESET, "and recv() fails with ECONNRESET");
		close(c);
	}

	{
		// a send on a socket shut down for sending fails with EPIPE too, and that's no reset
		int c, s;
		char b;
		check(connected_pair(lsn, &addr, kq, &c, &s), "set up a connection");
		shutdown(c, SHUT_WR);
		check(next_with(kq, c, NOTE_WRITECLOSED, &ev), "our shutdown(SHUT_WR) fires NOTE_WRITECLOSED");
		close(s);
		poll_for(c, POLLHUP);
		check(send(c, "x", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE, "a send after it fails with EPIPE");
		check(!next_with(kq, c, NOTE_CONNRESET, &ev), "without NOTE_CONNRESET");
		check(recv(c, &b, 1, 0) == 0, "and recv() finds end of file");
		close(c);
	}

	{
		// recv() takes the error before the knote looks: XNU posted NOTE_CONNRESET when the reset came
		int c, s;
		char b;
		check(connected_pair(lsn, &addr, kq, &c, &s), "set up a connection");
		reset(s);
		struct pollfd pfd = { c, POLLIN, 0 };
		poll(&pfd, 1, 2000);
		check(recv(c, &b, 1, 0) < 0 && errno == ECONNRESET, "recv() gets the reset first");
		check(next_with(kq, c, NOTE_CONNRESET, &ev), "the knote still fires NOTE_CONNRESET");
		check(!(ev.flags & EV_EOF), "without EV_EOF: the error was taken");
		close(c);
	}

	{
		// EVFILT_READ on a reset socket reports the error at end of file (filt_soread), with an EVFILT_SOCK knote
		// on the same socket
		int c, s;
		check(connected_pair(lsn, &addr, kq, &c, &s), "set up a connection");
		change(kq, c, EVFILT_READ, EV_ADD | EV_CLEAR, 0);
		reset(s);
		int got = 0;
		for (int i = 0; i < 10 && !got; i++) {
			struct timespec timeout = { 2, 0 };
			if (kevent(kq, NULL, 0, &ev, 1, &timeout) != 1)
				break;
			got = ev.ident == (uintptr_t) c && ev.filter == EVFILT_READ;
		}
		check(got && (ev.flags & EV_EOF) && ev.fflags == ECONNRESET, "EVFILT_READ's EV_EOF carries ECONNRESET");
		close(c);
	}

	{
		// and without one: EVFILT_READ alone takes the error from Linux, and leaves it for recv()
		int s, c = tcp_pair(lsn, &addr, &s);
		char b;
		change(kq, c, EVFILT_READ, EV_ADD | EV_CLEAR, 0);
		reset(s);
		int got = 0;
		for (int i = 0; i < 10 && !got; i++) {
			struct timespec timeout = { 2, 0 };
			if (kevent(kq, NULL, 0, &ev, 1, &timeout) != 1)
				break;
			got = ev.ident == (uintptr_t) c && ev.filter == EVFILT_READ;
		}
		check(got && (ev.flags & EV_EOF) && ev.fflags == ECONNRESET,
		    "EVFILT_READ's EV_EOF carries ECONNRESET without an EVFILT_SOCK knote");
		check(recv(c, &b, 1, 0) < 0 && errno == ECONNRESET, "and recv() still gets it");
		close(c);
	}

	{
		// a held error stays held however many sockets nobody watches have errors: they took table entries while
		// any knote existed, and the oldest entry went, held error and all (a refused connect then looked successful)
		int t = listener(&closed), others[SOCKETS_WITH_ERRORS], n = 0;
		int watched = socket(AF_INET, SOCK_STREAM, 0);
		char b;
		close(t);
		add(kq, watched, STATE_NOTES);
		int h = refused(&closed);
		check(send(h, "x", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE, "a send on a refused socket fails with EPIPE");
		for (; n < SOCKETS_WITH_ERRORS; n++) {
			others[n] = refused(&closed);
			recv(others[n], &b, 1, 0);
		}
		check(so_error(h) == ECONNREFUSED, "its error is still there after many other sockets' errors");
		while (n > 0)
			close(others[--n]);
		close(h);
		close(watched);
	}

	{
		// a socket shut down for sending: Linux tells no one, unless it's TCP (XNU's soshutdown() posts
		// SO_FILT_HINT_CANTSENDMORE whatever the protocol)
		int sv[2];
		socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
		check(add(kq, sv[0], STATE_NOTES) == 0 && next_with(kq, sv[0], NOTE_CONNECTED, &ev),
		    "a socket pair's socket is connected");
		shutdown(sv[0], SHUT_WR);
		check(next_with(kq, sv[0], NOTE_WRITECLOSED, &ev), "an AF_UNIX shutdown(SHUT_WR) fires NOTE_WRITECLOSED");
		close(sv[0]);
		close(sv[1]);
	}

	{
		// closing a kqueue drops its knotes, as XNU's kqueue_close() does: a knote kept its copy of the socket open,
		// so closing the socket afterwards sent the peer no FIN
		int other = kqueue(), s, c = tcp_pair(lsn, &addr, &s);
		char b;
		add(other, c, STATE_NOTES);
		close(other);
		close(c);
		check(poll_for(s, POLLIN) && recv(s, &b, 1, MSG_DONTWAIT) == 0,
		    "a socket closed after its kqueue closes the connection");
		close(s);
	}

	{
		// kevent() returns 0 only when its timeout has passed, though a knote it found had nothing to report: a
		// regular file at end of file (it returned 0 at once)
		FILE* f = tmpfile();
		struct timespec t0, t1, wait = { 0, 300000000 };
		change(kq, fileno(f), EVFILT_READ, EV_ADD, 0);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		int n = kevent(kq, NULL, 0, &ev, 1, &wait);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		long waited = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
		check(n == 0 && waited >= 250, "kevent() waits out its timeout on a file at end of file");
		change(kq, fileno(f), EVFILT_READ, EV_DELETE, 0);
		fclose(f);
	}

	{
		// a datagram socket: connect() raises NOTE_CONNECTED (XNU's soisconnected()), though Linux wakes no one
		int u = socket(AF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in to;
		int t = socket(AF_INET, SOCK_DGRAM, 0);
		socklen_t len = sizeof(to);
		memset(&to, 0, sizeof(to));
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bind(t, (struct sockaddr*) &to, sizeof(to));
		getsockname(t, (struct sockaddr*) &to, &len);
		close(t); // nothing listens on to any more: sending there draws ICMP port unreachable

		check(add(kq, u, STATE_NOTES) == 0, "EVFILT_SOCK registers on a datagram socket");
		connect(u, (struct sockaddr*) &to, sizeof(to));
		check(next_with(kq, u, NOTE_CONNECTED, &ev), "a datagram socket's connect() fires NOTE_CONNECTED");

		// its ICMP error stays the socket's: EVFILT_READ fires for it and recv() returns it, rather than blocking.
		// Both knotes look at the socket before recv() does, whichever kevent() returns first.
		change(kq, u, EVFILT_READ, EV_ADD | EV_CLEAR, 0);
		send(u, "x", 1, 0);
		check(poll_for(u, POLLERR), "a datagram socket's ICMP error arrives");
		int got = 0;
		struct timespec drain = { 0, 300000000 };
		while (kevent(kq, NULL, 0, &ev, 1, &drain) == 1)
			got |= ev.ident == (uintptr_t) u && ev.filter == EVFILT_READ;
		check(got, "EVFILT_READ fires for a datagram socket's error");
		struct pollfd pfd = { u, POLLIN, 0 };
		check(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLERR), "after the knotes looked, poll() still finds the error");
		char b;
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_alarm;
		sigaction(SIGALRM, &sa, NULL);
		alarm(2);
		check(recv(u, &b, 1, 0) < 0 && errno == ECONNREFUSED, "recv() returns the datagram socket's error");
		alarm(0);
		close(u);
	}

	{
		// XNU's sokqfilter only takes EVFILT_SOCK on sockets; a pipe's kqfilter refuses it
		int p[2];
		pipe(p);
		check(add(kq, p[0], STATE_NOTES) == EINVAL, "EVFILT_SOCK on a pipe fails with EINVAL");
		close(p[0]);
		close(p[1]);
	}

	close(lsn);
	close(kq);
	return failures;
}
