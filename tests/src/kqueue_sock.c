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
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/types.h>
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

static void connected_pair(int lsn, const struct sockaddr_in* addr, int kq, int* client, int* server)
{
	struct kevent ev;
	int err;

	*client = connecting(addr, &err);
	add(kq, *client, STATE_NOTES);
	next_with(kq, *client, NOTE_CONNECTED, &ev);
	*server = accept(lsn, NULL, NULL);
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
		struct linger lg = { 1, 0 };
		connected_pair(lsn, &addr, kq, &c, &s);
		setsockopt(s, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
		close(s);
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
