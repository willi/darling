// A PF_ROUTE socket answers RTM_GET with the interface Linux routes the address through, and that interface's index
// is the one getifaddrs() (NET_RT_IFLIST), if_nametoindex() and if_indextoname() give. The iOS Simulator's
// Network.framework finds each path's interface this way; Darling refused PF_ROUTE sockets, so every nw_path was
// "unsatisfied (No network route)" and NSURLSession requests failed.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

#define ROUNDUP32(a) ((a) > 0 ? (1 + (((a) - 1) | (sizeof(uint32_t) - 1))) : sizeof(uint32_t))

struct reply {
	struct rt_msghdr hdr;
	char addrs[512];
};

// RTM_GET for dst (and the interface, RTA_IFP), as Network.framework's nw_route_get_ifindex asks; returns 0 if
// write() took the whole request (-1 if not, -2 if no reply came), and the reply that has our sequence number in r
static long route_get(int s, const struct sockaddr* dst, int seq, struct reply* r)
{
	struct {
		struct rt_msghdr hdr;
		char addrs[128];
	} req;
	struct sockaddr_dl ifp = { .sdl_len = sizeof(ifp), .sdl_family = AF_LINK };
	char* p = req.addrs;
	long ret;

	memset(&req, 0, sizeof(req));
	memcpy(p, dst, dst->sa_len);
	p += ROUNDUP32(dst->sa_len);
	memcpy(p, &ifp, sizeof(ifp));
	p += ROUNDUP32(sizeof(ifp));
	req.hdr.rtm_msglen = p - (char*) &req;
	req.hdr.rtm_version = RTM_VERSION;
	req.hdr.rtm_type = RTM_GET;
	req.hdr.rtm_addrs = RTA_DST | RTA_IFP;
	req.hdr.rtm_seq = seq;

	ret = write(s, &req, req.hdr.rtm_msglen) == req.hdr.rtm_msglen ? 0 : -1;
	for (;;) {
		memset(r, 0, sizeof(*r));
		if (read(s, r, sizeof(*r)) < (long) sizeof(r->hdr))
			return -2;
		if (r->hdr.rtm_seq == seq && r->hdr.rtm_pid == getpid())
			return ret;
	}
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

int main(void)
{
	struct reply r;
	struct sockaddr_in anywhere = { .sin_len = sizeof(anywhere), .sin_family = AF_INET };
	char name[IF_NAMESIZE + 1] = "";
	char byindex[IF_NAMESIZE] = "";
	int s = socket(PF_ROUTE, SOCK_RAW, 0);

	check(s >= 0, "socket(PF_ROUTE, SOCK_RAW, 0)");
	if (s < 0)
		return failures;

	// a public address: the default route
	inet_pton(AF_INET, "1.1.1.1", &anywhere.sin_addr);
	long ret = route_get(s, (struct sockaddr*) &anywhere, 1, &r);
	check(ret == 0, "RTM_GET is written whole and answered");
	check(r.hdr.rtm_errno == 0 && (r.hdr.rtm_flags & (RTF_UP | RTF_DONE)) == (RTF_UP | RTF_DONE),
		"the reply is a usable route");
	check(r.hdr.rtm_index > 0, "the reply names an interface index");

	struct sockaddr_dl* sdl = (struct sockaddr_dl*) reply_addr(&r, RTA_IFP);
	check(sdl != NULL && sdl->sdl_family == AF_LINK && sdl->sdl_index == r.hdr.rtm_index && sdl->sdl_nlen > 0,
		"RTA_IFP is the interface's link address");
	if (sdl != NULL && sdl->sdl_nlen <= IF_NAMESIZE)
		memcpy(name, sdl->sdl_data, sdl->sdl_nlen);
	printf("# %s routes through %s (index %u)\n", "1.1.1.1", name, r.hdr.rtm_index);

	check(if_nametoindex(name) == r.hdr.rtm_index, "if_nametoindex() gives the same index");
	check(if_indextoname(r.hdr.rtm_index, byindex) != NULL && strcmp(byindex, name) == 0,
		"if_indextoname() gives the same name");

	struct ifaddrs *ifa, *i;
	int listed = 0;
	if (getifaddrs(&ifa) == 0) {
		for (i = ifa; i; i = i->ifa_next) {
			if (i->ifa_addr && i->ifa_addr->sa_family == AF_LINK && strcmp(i->ifa_name, name) == 0)
				listed = ((struct sockaddr_dl*) i->ifa_addr)->sdl_index == r.hdr.rtm_index;
		}
		freeifaddrs(ifa);
	}
	check(listed, "getifaddrs() (NET_RT_IFLIST) lists the interface with the same index");

	// the loopback address goes through the loopback interface
	inet_pton(AF_INET, "127.0.0.1", &anywhere.sin_addr);
	ret = route_get(s, (struct sockaddr*) &anywhere, 2, &r);
	sdl = (struct sockaddr_dl*) reply_addr(&r, RTA_IFP);
	check(ret == 0 && sdl != NULL && sdl->sdl_type == 0x18 /* IFT_LOOP */, "127.0.0.1 routes through the loopback");

	// only RTM_GET: the routes are Linux's
	struct {
		struct rt_msghdr hdr;
		struct sockaddr_in dst;
	} add = { .hdr = { .rtm_msglen = sizeof(add), .rtm_version = RTM_VERSION, .rtm_type = RTM_ADD,
		.rtm_addrs = RTA_DST, .rtm_seq = 3 }, .dst = anywhere };
	check(write(s, &add, sizeof(add)) == -1 && errno == EPERM, "RTM_ADD fails with EPERM");

	// a closed routing socket's number, reused, is an ordinary descriptor again
	close(s);
	int sv[2];
	char c = 0;
	check(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0 && sv[0] == s && write(sv[0], "x", 1) == 1
		&& read(sv[1], &c, 1) == 1 && c == 'x', "a reused descriptor number writes normally");

	return failures;
}
