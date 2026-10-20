/*
 * simasuid: runs a program as another user, as CoreSimulator runs a device's launchd_sim as the user who boots the
 * device. simboot.sh starts the device's launchd_sim_trampoline through it (SIM_UID, SIM_GID; tools/ios-sim/README.md
 * says why).
 *
 * Usage: simasuid <uid> <gid> <program> [args...]
 *
 * The groups become just <gid>, as a Mac user's processes have their primary group (staff, 20, for the first user):
 * none of the caller's (root's) stays. Darling keeps the ids in darlingserver; the Linux process stays as it was.
 *
 * First it starts an audit session for <uid>, as a Mac's login does for the user who boots the device: audit ID <uid>,
 * a session ID the kernel assigns (from 100000 up), terminal ID none. CoreSimulator runs the device's launchd_sim in
 * that session, so every device process carries it in its audit token (auid 501 and the session's asid, where a
 * process outside any session has AU_DEFAUDITID). Only root can set one, so it comes before the uid. Each simasuid
 * starts its own session, where a Mac's host-side clients all share the login's. The device's own processes share one
 * (launchd_sim's); whether anything compares a device process's session ID with a host-side client's hasn't been
 * checked, and isn't known to matter.
 *
 * A device booted as root (uid 0 and gid 0) doesn't go through simasuid (simboot.sh adds it only for another uid or
 * gid), so its processes run outside any session, as a Mac's system daemons do.
 *
 * From willi/kage's sim/asuid (d8c0d15), which runs a Mac program as a device's user from inside a device. This one
 * is a Mac program itself, so it leaves DYLD_ROOT_PATH as it is.
 */
#include <bsm/audit.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// a uid or gid: 0 to INT_MAX, as Darling keeps them (a negative one means "no change" to darlingserver)
static int parse_id(const char* s, long* id)
{
	char* end;
	errno = 0;
	*id = strtol(s, &end, 10);
	return errno == 0 && end != s && *end == '\0' && *id >= 0 && *id <= INT_MAX;
}

int main(int argc, char** argv)
{
	long uid, gid;

	if (argc < 4)
	{
		fprintf(stderr, "usage: %s <uid> <gid> <program> [args...]\n", argv[0]);
		return 64;
	}
	if (!parse_id(argv[1], &uid) || !parse_id(argv[2], &gid))
	{
		fprintf(stderr, "simasuid: not a uid and gid: %s %s\n", argv[1], argv[2]);
		return 64;
	}
	// the session first, then the groups and the gid: a process that isn't root can change none of them after
	auditinfo_addr_t session = {
		.ai_auid = (au_id_t) uid,
		.ai_termid = { .at_type = AU_IPv4 },
		.ai_asid = AU_ASSIGN_ASID,
	};
	if (setaudit_addr(&session, sizeof(session)) != 0)
	{
		fprintf(stderr, "simasuid: can't start an audit session for uid %ld: %s\n", uid, strerror(errno));
		return 1;
	}
	gid_t groups[1] = { (gid_t) gid };
	if (setgroups(1, groups) != 0 || setgid((gid_t) gid) != 0 || setuid((uid_t) uid) != 0)
	{
		fprintf(stderr, "simasuid: can't become uid %ld gid %ld: %s\n", uid, gid, strerror(errno));
		return 1;
	}
	execv(argv[3], argv + 3);
	fprintf(stderr, "simasuid: %s: %s\n", argv[3], strerror(errno));
	return 127;
}
