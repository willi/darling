/*
 * simasuid: runs a program as another user, as CoreSimulator runs a device's launchd_sim as the user who boots the
 * device (never root). simboot.sh starts the device's launchd_sim_trampoline through it when SIM_UID is set.
 *
 * Usage: simasuid <uid> <program> [args...]
 *
 * The group is the uid's number too (a Mac's first user is uid 501, gid 20; the simulator's processes only ever ask
 * for their uid). Darling keeps the uid in darlingserver: the Linux process stays as it was.
 *
 * From willi/kage's sim/asuid (d8c0d15), which runs a Mac program as a device's user from inside a device. This one
 * is a Mac program itself, so it leaves DYLD_ROOT_PATH as it is.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv)
{
	char* end;
	long uid;

	if (argc < 3)
	{
		fprintf(stderr, "usage: %s <uid> <program> [args...]\n", argv[0]);
		return 64;
	}
	errno = 0;
	uid = strtol(argv[1], &end, 10);
	if (errno != 0 || *end != '\0' || end == argv[1] || uid < 0 || uid > 0x7fffffff)
	{
		fprintf(stderr, "simasuid: not a uid: %s\n", argv[1]);
		return 64;
	}
	// the group first: a process that isn't root can't change it after
	if (setgid((gid_t) uid) != 0 || setuid((uid_t) uid) != 0)
	{
		fprintf(stderr, "simasuid: setuid(%ld): %s\n", uid, strerror(errno));
		return 1;
	}
	execv(argv[2], argv + 2);
	fprintf(stderr, "simasuid: %s: %s\n", argv[2], strerror(errno));
	return 127;
}
