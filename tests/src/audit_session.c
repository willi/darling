// A process's audit token and audit session, as XNU keeps them (bsd/kern/kern_prot.c proc_calc_audit_token(),
// bsd/security/audit/audit_syscalls.c setaudit_addr()): darlingserver fills the token's auid and asid from the
// session that setaudit_addr() set, its real ids from the uid and gid, and the message trailer's sender from the
// same ids; the session is root's to set and is inherited at fork and exec. A device's processes carry auid 501 and a
// session ID from 100000 up, as on a Mac (B33: before this, auid, ruid, rgid and asid were 0 and the session was a
// per-process copy that exec dropped).
//
// Run as root. Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <bsm/audit.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

// the token's layout, as XNU's proc_calc_audit_token() fills it (libbsm's audit_token_to_*() read the same words)
#define TOKEN_AUID(t) ((t).val[0])
#define TOKEN_EUID(t) ((t).val[1])
#define TOKEN_EGID(t) ((t).val[2])
#define TOKEN_RUID(t) ((t).val[3])
#define TOKEN_RGID(t) ((t).val[4])
#define TOKEN_PID(t) ((int) (t).val[5])
#define TOKEN_ASID(t) ((int) (t).val[6])

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	fflush(stdout);
	if (!ok)
		failures++;
}

static audit_token_t own_token(void)
{
	audit_token_t token;
	mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
	memset(&token, 0, sizeof(token));
	task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t) &token, &count);
	return token;
}

// the trailer of a message this process sends itself: what a receiver (libxpc's xpc_connection_get_audit_token(),
// MIG's ServerSecToken) sees of the sender
static int own_trailer(audit_token_t* audit, security_token_t* sender)
{
	mach_port_t port;
	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS)
		return -1;
	struct {
		mach_msg_header_t header;
		mach_msg_audit_trailer_t trailer;
	} msg;
	memset(&msg, 0, sizeof(msg));
	msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
	msg.header.msgh_size = sizeof(msg.header);
	msg.header.msgh_remote_port = port;
	kern_return_t kr = mach_msg(&msg.header, MACH_SEND_MSG, sizeof(msg.header), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
	if (kr == KERN_SUCCESS)
		kr = mach_msg(&msg.header, MACH_RCV_MSG | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
				MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT), 0, sizeof(msg), port, 0, MACH_PORT_NULL);
	mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
	if (kr != KERN_SUCCESS)
		return -1;
	*audit = msg.trailer.msgh_audit;
	*sender = msg.trailer.msgh_sender;
	return 0;
}

// the child half: the process simasuid would make, after exec. argv: self child ASID
static int child(int asid)
{
	audit_token_t t = own_token();
	auditinfo_addr_t aia;
	au_id_t auid = 12345;
	memset(&aia, 0, sizeof(aia));

	check(TOKEN_AUID(t) == 501, "after exec: token auid 501");
	check(TOKEN_EUID(t) == 501 && TOKEN_EGID(t) == 20, "after exec: token euid 501, egid 20");
	// the regression guard (B33): the real ids stayed 0
	check(TOKEN_RUID(t) == 501 && TOKEN_RGID(t) == 20, "after exec: token ruid 501, rgid 20");
	check(TOKEN_PID(t) == getpid(), "after exec: token pid is this process's");
	check(TOKEN_ASID(t) == asid, "after exec: token asid is the session the parent started");
	check(getaudit_addr(&aia, sizeof(aia)) == 0 && aia.ai_auid == 501 && aia.ai_asid == asid,
		"after exec: getaudit_addr() agrees with the token");
	check(aia.ai_mask.am_success == ~0u && aia.ai_mask.am_failure == ~0u,
		"after exec: a process that isn't root sees the masks as ~0");
	check(getauid(&auid) == 0 && auid == 501, "after exec: getauid() 501");

	audit_token_t ta;
	security_token_t st;
	int got = own_trailer(&ta, &st);
	check(got == 0 && memcmp(&ta, &t, sizeof(t)) == 0, "the message trailer's audit token is the task's");
	// msgh_sender was the kernel's (0, 0) for every process
	check(got == 0 && st.val[0] == 501 && st.val[1] == 20, "the message trailer's sender is uid 501, gid 20");

	aia.ai_auid = 0;
	aia.ai_asid = AU_ASSIGN_ASID;
	errno = 0;
	check(setaudit_addr(&aia, sizeof(aia)) == -1 && errno == EPERM, "a process that isn't root can't set a session");
	return failures;
}

int main(int argc, char** argv)
{
	if (argc == 3 && strcmp(argv[1], "child") == 0)
		return child(atoi(argv[2]));

	char self[MAXPATHLEN];
	uint32_t n = sizeof(self);
	_NSGetExecutablePath(self, &n);

	if (geteuid() != 0)
	{
		printf("ok - # SKIP needs root\n");
		return 0;
	}

	// this process's token and session agree, whatever the session is
	audit_token_t t = own_token();
	auditinfo_addr_t aia;
	memset(&aia, 0, sizeof(aia));
	check(getaudit_addr(&aia, sizeof(aia)) == 0, "getaudit_addr()");
	check(TOKEN_AUID(t) == aia.ai_auid && TOKEN_ASID(t) == aia.ai_asid,
		"the token's auid and asid are getaudit_addr()'s");

	// a bad terminal type, before anything else
	auditinfo_addr_t bad = aia;
	bad.ai_termid.at_type = 7;
	errno = 0;
	check(setaudit_addr(&bad, sizeof(bad)) == -1 && errno == EINVAL, "a terminal type that isn't IPv4 or IPv6: EINVAL");

	// a new session for 501, as simasuid starts one
	auditinfo_addr_t s;
	memset(&s, 0, sizeof(s));
	s.ai_auid = 501;
	s.ai_termid.at_type = AU_IPv4;
	s.ai_asid = AU_ASSIGN_ASID;
	check(setaudit_addr(&s, sizeof(s)) == 0, "root sets a new session");
	check(s.ai_asid >= 100000, "the assigned session ID is 100000 or more, as XNU's");
	check(s.ai_auid == 501, "setaudit_addr() hands back the session it assigned");
	t = own_token();
	check(TOKEN_AUID(t) == 501 && TOKEN_ASID(t) == s.ai_asid, "the token follows the session");

	// the session's audit ID is immutable once set
	auditinfo_addr_t again = s;
	again.ai_auid = 502;
	errno = 0;
	check(setaudit_addr(&again, sizeof(again)) == -1 && errno == EINVAL, "an existing session's audit ID can't change");

	// a fork keeps it, and so does the exec that follows a setgid/setuid to 501
	char asid[16];
	snprintf(asid, sizeof(asid), "%d", s.ai_asid);
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0)
	{
		if (setgid(20) != 0 || setuid(501) != 0)
			_exit(100);
		execl(self, self, "child", asid, (char*) NULL);
		_exit(101);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) < 100, "the child became 501 and ran");
	if (WIFEXITED(status))
		failures += WEXITSTATUS(status) < 100 ? WEXITSTATUS(status) : 0;
	return failures;
}
