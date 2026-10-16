// A PF_ROUTE socket answers RTM_GET with the interface Linux routes the address through, and that interface's index
// is the one getifaddrs() (NET_RT_IFLIST), if_nametoindex() and if_indextoname() give. The iOS Simulator's
// Network.framework finds each path's interface this way (FIONBIO, write RTM_GET, read); Darling refused PF_ROUTE
// sockets, so every nw_path was "unsatisfied (No network route)" and NSURLSession requests failed.
//
// Prints one "ok"/"not ok" line per check (and "# SKIP" for checks this network can't exercise); the exit status is
// the number of failed checks.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

static void skip(const char* what, const char* why)
{
	printf("ok - %s # SKIP %s\n", what, why);
}

// XNU's private RTM_GET that only the sender sees (net/route.h, PRIVATE)
#define RTM_GET_SILENT 0x11

#define ROUNDUP32(a) ((a) > 0 ? (1 + (((a) - 1) | (sizeof(uint32_t) - 1))) : sizeof(uint32_t))

struct reply {
	struct rt_msghdr hdr;
	char addrs[512];
};

struct request {
	struct rt_msghdr hdr;
	char addrs[128];
};

// RTM_GET for dst and its interface (RTA_IFP), as Network.framework's nw_route_get_ifindex asks
static void make_get(struct request* req, const struct sockaddr* dst, int seq)
{
	struct sockaddr_dl ifp = { .sdl_len = sizeof(ifp), .sdl_family = AF_LINK };
	char* p = req->addrs;

	memset(req, 0, sizeof(*req));
	memcpy(p, dst, dst->sa_len);
	p += ROUNDUP32(dst->sa_len);
	memcpy(p, &ifp, sizeof(ifp));
	p += ROUNDUP32(sizeof(ifp));
	req->hdr.rtm_msglen = p - (char*) req;
	req->hdr.rtm_version = RTM_VERSION;
	req->hdr.rtm_type = RTM_GET;
	req->hdr.rtm_addrs = RTA_DST | RTA_IFP;
	req->hdr.rtm_seq = seq;
}

// The next message with sequence number seq, waiting at most a second for each; 0, or -1 if none came. The socket is
// nonblocking, so a reply that never comes fails the check instead of hanging the test.
static int read_reply(int s, int seq, struct reply* r)
{
	struct pollfd pfd = { .fd = s, .events = POLLIN };

	for (;;) {
		if (poll(&pfd, 1, 1000) != 1)
			return -1;
		memset(r, 0, sizeof(*r));
		if (read(s, r, sizeof(*r)) < (long) sizeof(r->hdr))
			return -1;
		if (r->hdr.rtm_seq == seq)
			return 0;
	}
}

// writes req as it is; returns the reply's rtm_errno, or -1 if it wasn't answered
static int route_get_raw(int s, struct request* req, int seq, struct reply* r)
{
	write(s, req, req->hdr.rtm_msglen);
	return read_reply(s, seq, r) < 0 ? -1 : r->hdr.rtm_errno;
}

// RTM_GET dst; returns the reply's rtm_errno (0: a route), or -1 if the request wasn't written whole and answered
static int route_get(int s, const struct sockaddr* dst, int seq, struct reply* r)
{
	struct request req;

	make_get(&req, dst, seq);
	long n = write(s, &req, req.hdr.rtm_msglen);
	int err = n < 0 ? errno : 0;
	if (read_reply(s, seq, r) < 0 || (n != req.hdr.rtm_msglen && err != r->hdr.rtm_errno))
		return -1;
	return r->hdr.rtm_errno;
}

// the reply's sockaddr for bit (RTA_*), or NULL
static struct sockaddr* reply_addr(struct reply* r, int bit)
{
	char* p = r->addrs;
	for (int b = 1; b < 0x100; b <<= 1) {
		if (!(r->hdr.rtm_addrs & b))
			continue;
		struct sockaddr* sa = (struct sockaddr*) p;
		if (b == bit)
			return sa;
		p += ROUNDUP32(sa->sa_len);
	}
	return NULL;
}

static int routing_socket(void)
{
	int one = 1;
	int s = socket(PF_ROUTE, SOCK_RAW, 0);
	if (s >= 0)
		ioctl(s, FIONBIO, &one);
	return s;
}

static int answers(int s, int seq)
{
	struct sockaddr_in lo = { .sin_len = sizeof(lo), .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	struct reply r;
	return route_get(s, (struct sockaddr*) &lo, seq, &r) == 0 && r.hdr.rtm_pid == getpid();
}

// The interface name and index agree with if_nametoindex(), if_indextoname() and getifaddrs()
static void check_agreement(const char* name, unsigned index)
{
	char byindex[IF_NAMESIZE] = "";
	struct ifaddrs *ifa, *i;
	int listed = 0;

	check(if_nametoindex(name) == index, "if_nametoindex() gives the same index");
	check(if_indextoname(index, byindex) != NULL && strcmp(byindex, name) == 0, "if_indextoname() gives the same name");
	if (getifaddrs(&ifa) == 0) {
		for (i = ifa; i; i = i->ifa_next) {
			if (i->ifa_addr && i->ifa_addr->sa_family == AF_LINK && strcmp(i->ifa_name, name) == 0)
				listed = ((struct sockaddr_dl*) i->ifa_addr)->sdl_index == index;
		}
		freeifaddrs(ifa);
	}
	check(listed, "getifaddrs() (NET_RT_IFLIST) lists the interface with the same index");
}

// getifaddrs() lists every interface if_nameindex() names, with the same index, and their flags
static void check_interfaces(void)
{
	struct if_nameindex* names = if_nameindex();
	struct ifaddrs *ifa, *i;
	int all = names != NULL, loopback = 0, up = 1;

	if (getifaddrs(&ifa) != 0) {
		check(0, "getifaddrs()");
		return;
	}
	for (struct if_nameindex* n = names; n && n->if_index; n++) {
		int found = 0;
		for (i = ifa; i; i = i->ifa_next) {
			if (i->ifa_addr && i->ifa_addr->sa_family == AF_LINK && strcmp(i->ifa_name, n->if_name) == 0
					&& ((struct sockaddr_dl*) i->ifa_addr)->sdl_index == n->if_index) {
				found = 1;
				loopback |= (i->ifa_flags & IFF_LOOPBACK) != 0;
				up &= (i->ifa_flags & IFF_UP) != 0 || strcmp(n->if_name, "lo") != 0;
			}
		}
		all &= found;
	}
	freeifaddrs(ifa);
	if_freenameindex(names);
	check(all, "getifaddrs() lists every interface if_nameindex() names, by the same index");
	check(loopback && up, "the loopback interface is IFF_LOOPBACK and IFF_UP");
}

int main(void)
{
	struct reply r;
	struct sockaddr_in in4 = { .sin_len = sizeof(in4), .sin_family = AF_INET };
	struct sockaddr_in6 in6 = { .sin6_len = sizeof(in6), .sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT };
	char name[IF_NAMESIZE + 1] = "";
	struct request req;
	int s = routing_socket();
	int seq = 1;

	// each line out before a crash can lose it
	setvbuf(stdout, NULL, _IOLBF, 0);
	check(s >= 0, "socket(PF_ROUTE, SOCK_RAW, 0)");
	if (s < 0)
		return failures;
	check(read(s, &r, sizeof(r)) == -1 && errno == EAGAIN, "FIONBIO: a read with nothing queued fails with EAGAIN");

	// a public address: the default route's interface
	inet_pton(AF_INET, "1.1.1.1", &in4.sin_addr);
	int err = route_get(s, (struct sockaddr*) &in4, seq++, &r);
	int routed = err == 0;
	check(err >= 0, "RTM_GET is answered");
	if (routed) {
		// the route XNU reports is the table entry that matched: the default route, not a host route to 1.1.1.1
		struct sockaddr* mask = reply_addr(&r, RTA_NETMASK);
		check(!(r.hdr.rtm_flags & RTF_HOST) && (r.hdr.rtm_flags & RTF_GATEWAY) && mask != NULL && mask->sa_len == 0,
			"1.1.1.1's route is the default route: a gateway, and a zero-length mask");
		unsigned short index = r.hdr.rtm_index;
		struct sockaddr_in any = { .sin_len = sizeof(any), .sin_family = AF_INET };
		struct reply d;
		// regression: Linux answers 0.0.0.0 with the loopback, so nw_path's interface was lo
		check(route_get(s, (struct sockaddr*) &any, seq++, &d) == 0 && d.hdr.rtm_index == index
				&& !(d.hdr.rtm_flags & RTF_LOCAL), "0.0.0.0 matches the default route, as in XNU");
	} else if (err == ESRCH) {
		skip("the default route's interface", "no default route");
		// the loopback's agreement instead
		in4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		err = route_get(s, (struct sockaddr*) &in4, seq++, &r);
	}
	check(err == 0 && (r.hdr.rtm_flags & (RTF_UP | RTF_DONE)) == (RTF_UP | RTF_DONE) && r.hdr.rtm_pid == getpid(),
		"the reply is a usable route, from this process");
	check(r.hdr.rtm_index > 0, "the reply names an interface index");
	struct sockaddr_dl* sdl = (struct sockaddr_dl*) reply_addr(&r, RTA_IFP);
	check(sdl != NULL && sdl->sdl_family == AF_LINK && sdl->sdl_index == r.hdr.rtm_index && sdl->sdl_nlen > 0
			&& sdl->sdl_nlen <= IF_NAMESIZE, "RTA_IFP is the interface's link address");
	if (sdl != NULL && sdl->sdl_nlen <= IF_NAMESIZE)
		memcpy(name, sdl->sdl_data, sdl->sdl_nlen);
	printf("# routes through %s (index %u)\n", name, r.hdr.rtm_index);
	check_agreement(name, r.hdr.rtm_index);
	check_interfaces();

	// the loopback addresses go through the loopback interface
	in4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sdl = route_get(s, (struct sockaddr*) &in4, seq++, &r) == 0 ? (struct sockaddr_dl*) reply_addr(&r, RTA_IFP) : NULL;
	check(sdl != NULL && sdl->sdl_type == 0x18 /* IFT_LOOP */, "127.0.0.1 routes through the loopback");
	check(r.hdr.rtm_flags & RTF_LOCAL, "and its route is RTF_LOCAL");
	err = route_get(s, (struct sockaddr*) &in6, seq++, &r);
	if (err == ESRCH) {
		skip("::1 routes through the loopback", "no IPv6 in this network namespace");
	} else {
		sdl = err == 0 ? (struct sockaddr_dl*) reply_addr(&r, RTA_IFP) : NULL;
		check(sdl != NULL && sdl->sdl_type == 0x18, "::1 routes through the loopback");
		struct ifaddrs *ifa, *i;
		int listed = 0;
		if (getifaddrs(&ifa) == 0) {
			for (i = ifa; i; i = i->ifa_next) {
				listed |= i->ifa_addr && i->ifa_addr->sa_family == AF_INET6 && (i->ifa_flags & IFF_LOOPBACK)
					&& IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6*) i->ifa_addr)->sin6_addr);
			}
			freeifaddrs(ifa);
		}
		// NET_RT_IFLIST listed IPv4 addresses only
		check(listed, "getifaddrs() lists ::1 on the loopback interface");
	}

	// only RTM_GET: the routes are Linux's. The refused request comes back with rtm_errno set.
	struct {
		struct rt_msghdr hdr;
		struct sockaddr_in dst;
	} add = { .hdr = { .rtm_msglen = sizeof(add), .rtm_version = RTM_VERSION, .rtm_type = RTM_ADD,
		.rtm_addrs = RTA_DST, .rtm_seq = seq }, .dst = in4 };
	check(write(s, &add, sizeof(add)) == -1 && errno == EPERM, "RTM_ADD fails with EPERM");
	check(read_reply(s, seq++, &r) == 0 && r.hdr.rtm_type == RTM_ADD && r.hdr.rtm_errno == EPERM,
		"and its echo carries rtm_errno EPERM");

	// a request whose rtm_msglen isn't its length fails, and comes back as it was sent
	make_get(&req, (struct sockaddr*) &in4, seq);
	check(write(s, &req, req.hdr.rtm_msglen - 4) == -1 && errno == EINVAL && read_reply(s, seq++, &r) == 0
			&& r.hdr.rtm_errno == 0 && r.hdr.rtm_type == RTM_GET, "a truncated request fails with EINVAL, echoed as is");
	// the addresses end with the message: the IFP that RTA_IFP announces may be left out, as XNU's rt_xaddrs() allows
	make_get(&req, (struct sockaddr*) &in4, seq);
	req.hdr.rtm_msglen = sizeof(req.hdr) + sizeof(in4);
	check(route_get_raw(s, &req, seq++, &r) == 0, "a request that leaves out its last address is answered");
	// an address that runs past the end of the message
	make_get(&req, (struct sockaddr*) &in4, seq);
	req.hdr.rtm_addrs = RTA_DST;
	req.hdr.rtm_msglen = sizeof(req.hdr) + 4;
	check(write(s, &req, req.hdr.rtm_msglen) == -1 && errno == EINVAL && read_reply(s, seq++, &r) == 0
			&& r.hdr.rtm_errno == EINVAL, "a truncated address fails with EINVAL, echoed");

	// the iovec paths: a request split across iovecs, and iovecs the kernel would refuse
	make_get(&req, (struct sockaddr*) &in4, seq);
	struct iovec split[2] = { { &req, sizeof(req.hdr) }, { req.addrs, req.hdr.rtm_msglen - sizeof(req.hdr) } };
	check(writev(s, split, 2) == req.hdr.rtm_msglen && read_reply(s, seq++, &r) == 0 && r.hdr.rtm_errno == 0,
		"writev() of a header and its addresses is answered");
	make_get(&req, (struct sockaddr*) &in4, seq);
	struct msghdr mh = { .msg_iov = split, .msg_iovlen = 2 };
	check(sendmsg(s, &mh, 0) == req.hdr.rtm_msglen && read_reply(s, seq++, &r) == 0 && r.hdr.rtm_errno == 0,
		"sendmsg() is answered");
	// regression: a length that wraps the sum used to pass the size check and overflow the stack
	struct iovec wrap[2] = { { &req, 100 }, { req.addrs, (size_t) -100 } };
	check(writev(s, wrap, 2) == -1 && errno == EINVAL, "iovec lengths that wrap fail with EINVAL");
	check(writev(s, split, 1025) == -1 && errno == EINVAL, "more than UIO_MAXIOV iovecs fail with EINVAL");
	struct iovec bad[2] = { { &req, sizeof(req.hdr) }, { (void*) 16, req.hdr.rtm_msglen - sizeof(req.hdr) } };
	check(writev(s, bad, 2) == -1 && errno == EFAULT, "an iovec at a bad address fails with EFAULT");
	check(write(s, (void*) 16, sizeof(req)) == -1 && errno == EFAULT, "a request at a bad address fails with EFAULT");

	// a routing socket as a socket: its type, options and names are XNU's
	int value = 0;
	socklen_t value_len = sizeof(value);
	check(getsockopt(s, SOL_SOCKET, SO_TYPE, &value, &value_len) == 0 && value == SOCK_RAW, "SO_TYPE is SOCK_RAW");
	value_len = sizeof(value);
	check(getsockopt(s, SOL_SOCKET, SO_USELOOPBACK, &value, &value_len) == 0 && value != 0,
		"SO_USELOOPBACK is on");
	value = 0;
	check(setsockopt(s, SOL_SOCKET, SO_USELOOPBACK, &value, sizeof(value)) == 0, "SO_USELOOPBACK can be turned off");
	make_get(&req, (struct sockaddr*) &in4, seq);
	check(write(s, &req, req.hdr.rtm_msglen) == req.hdr.rtm_msglen && read_reply(s, seq++, &r) < 0,
		"and then the sender's own request isn't answered to it");
	make_get(&req, (struct sockaddr*) &in4, seq++);
	req.hdr.rtm_type = RTM_GET_SILENT;
	check(write(s, &req, req.hdr.rtm_msglen) == -1 && errno == EINVAL, "nor is RTM_GET_SILENT allowed");
	value = 1;
	setsockopt(s, SOL_SOCKET, SO_USELOOPBACK, &value, sizeof(value));
	make_get(&req, (struct sockaddr*) &in4, seq);
	req.hdr.rtm_type = RTM_GET_SILENT;
	check(route_get_raw(s, &req, seq++, &r) == 0 && r.hdr.rtm_type == RTM_GET, "RTM_GET_SILENT is answered as RTM_GET");
	check(setsockopt(s, PF_ROUTE, 1 /* FreeBSD's ROUTE_MSGFILTER */, &value, sizeof(value)) == -1
			&& errno == ENOPROTOOPT, "an option of the routing level fails with ENOPROTOOPT");
	struct sockaddr_storage ss;
	socklen_t ss_len = sizeof(ss);
	check(getsockname(s, (struct sockaddr*) &ss, &ss_len) == -1 && errno == EINVAL, "getsockname() fails with EINVAL");
	ss_len = sizeof(ss);
	check(getpeername(s, (struct sockaddr*) &ss, &ss_len) == 0 && ss.ss_family == PF_ROUTE,
		"getpeername() gives the routing domain");
	make_get(&req, (struct sockaddr*) &in4, seq);
	check(sendto(s, &req, req.hdr.rtm_msglen, 0, (struct sockaddr*) &in4, sizeof(in4)) == -1 && errno == EISCONN,
		"sendto() with an address fails with EISCONN");
	check(socket(PF_ROUTE, SOCK_DGRAM, 0) == -1 && errno == EPROTONOSUPPORT,
		"socket(PF_ROUTE, SOCK_DGRAM) fails with EPROTONOSUPPORT");

	// regression: the answering end is the process's. A program that closes every descriptor it didn't open (as
	// daemons do) must not take it, or every later request goes unanswered.
	for (int fd = 3; fd < 256; fd++) {
		if (fd != s)
			close(fd);
	}
	check(answers(s, seq++), "closing every other descriptor leaves the routing socket answering");

	// a forked child shares the socket and asks through it too
	pid_t child = fork();
	if (child == 0)
		_exit(answers(s, 1000) ? 0 : 1);
	int status = -1;
	check(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"a forked child's RTM_GET is answered");

	// a copy of a routing socket is the same socket: it's answered too, also once the original is closed
	int copy = dup(s);
	check(copy >= 0 && answers(copy, seq++), "a dup() of a routing socket is answered");
	close(s);
	check(answers(copy, seq++), "and still is once the original is closed");
	s = copy;

	// a routing socket's number, replaced by dup2() or reused after close(), is an ordinary descriptor again
	int sv[2];
	char c = 0;
	check(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0 && dup2(sv[0], s) == s && write(s, "x", 1) == 1
			&& read(sv[1], &c, 1) == 1 && c == 'x', "a socket dup2()'d over a routing socket writes normally");
	close(s);
	close(sv[0]);
	close(sv[1]);
	s = routing_socket();
	close(s);
	check(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0 && (sv[0] == s || sv[1] == s) && write(s, "y", 1) == 1
			&& read(sv[0] == s ? sv[1] : sv[0], &c, 1) == 1 && c == 'y',
		"a closed routing socket's reused number writes normally");

	return failures;
}
