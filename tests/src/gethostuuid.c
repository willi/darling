// gethostuuid() as on XNU: the host's UUID, the same for every process (an iOS Simulator's processes get their Mac's).
// Darling read it from /etc/machine-id and failed with ENOSYS when that was empty, as a container's often is
// (Debian's images ship it empty): installd, lsd and SpringBoard got ENOSYS, and SQLite's proxy locking names its
// lock's host with it.
//
// The UUID is checked against its source, read here: the host's machine ID, hashed (machine-id(5)), or else the
// prefix's own ID, made up once. Not covered: making the prefix's ID while other processes do (it would mean removing
// the live prefix's ID from under its daemons).
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <CommonCrypto/CommonHMAC.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

extern char** environ;

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

// A machine ID (machine-id(5): 32 hexadecimal digits and a newline, not all zeros), or -1
static int read_id(const char* path, uuid_t id)
{
	char text[40] = { 0 }, hex[3] = { 0 };
	int fd = open(path, O_RDONLY), any = 0;
	ssize_t n = fd < 0 ? -1 : read(fd, text, sizeof(text) - 1);

	if (fd >= 0)
		close(fd);
	if (n != 32 && !(n == 33 && text[32] == '\n'))
		return -1;
	for (int i = 0; i < 16; i++) {
		char* end;
		memcpy(hex, text + i * 2, 2);
		id[i] = (unsigned char) strtoul(hex, &end, 16);
		if (*end != '\0')
			return -1;
		any |= id[i];
	}
	return any ? 0 : -1;
}

// What the UUID must be, and where it comes from
static const char* expected_uuid(uuid_t expected)
{
	// Golden master: Darling's application ID for the hash. Changing it changes every Darling host's UUID.
	static const unsigned char app_id[16] = {
		0xae, 0xcc, 0x84, 0x54, 0x64, 0xdd, 0x41, 0xad, 0xa1, 0x3c, 0x55, 0xf0, 0xbd, 0xea, 0x2e, 0x24,
	};
	unsigned char mac[CC_SHA256_DIGEST_LENGTH];
	uuid_t host;

	if (read_id("/Volumes/SystemRoot/etc/machine-id", host) == 0
	    || read_id("/Volumes/SystemRoot/var/lib/dbus/machine-id", host) == 0) {
		// systemd's sd_id128_get_machine_app_specific()
		CCHmac(kCCHmacAlgSHA256, host, sizeof(host), app_id, sizeof(app_id), mac);
		memcpy(expected, mac, 16);
		expected[6] = (expected[6] & 0x0f) | 0x40;
		expected[8] = (expected[8] & 0x3f) | 0x80;
		return "the host's machine ID, hashed";
	}
	if (read_id("/private/var/db/darling-hostuuid", expected) == 0)
		return "the prefix's ID";
	return NULL;
}

// another process's answer: this program run again with mode, printing the UUID it gets or the errno
static int run_self(const char* self, const char* mode, char* out, size_t size)
{
	int p[2], status = -1;
	pid_t pid;
	size_t got = 0;
	ssize_t n;
	char* argv[] = { (char*) self, (char*) mode, NULL };
	posix_spawn_file_actions_t fa;

	if (pipe(p) != 0)
		return -1;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	posix_spawn_file_actions_addclose(&fa, p[0]);
	int err = posix_spawn(&pid, self, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(p[1]);
	while (err == 0 && got < size - 1 && (n = read(p[0], out + got, size - 1 - got)) > 0)
		got += n;
	out[got] = '\0';
	close(p[0]);
	if (err != 0 || waitpid(pid, &status, 0) != pid)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(int argc, char** argv)
{
	struct timespec wait = { 1, 0 };
	uuid_t uuid, again, other, expected;
	char text[64];

	if (argc > 1 && strcmp(argv[1], "print") == 0) {
		if (gethostuuid(uuid, &wait) != 0)
			return 1;
		uuid_unparse(uuid, text);
		fputs(text, stdout);
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "faults") == 0) {
		// a fresh process's first calls: XNU copies the timeout in before it looks for the UUID, and the UUID out after
		int a = gethostuuid(uuid, NULL) == -1 && errno == EFAULT;
		int b = gethostuuid(uuid, (struct timespec*) 8) == -1 && errno == EFAULT;
		int c = gethostuuid((unsigned char*) 8, &wait) == -1 && errno == EFAULT;
		printf("%d%d%d", a, b, c);
		return 0;
	}

	setvbuf(stdout, NULL, _IOLBF, 0);

	check(run_self(argv[0], "faults", text, sizeof(text)) == 0 && strcmp(text, "111") == 0,
	    "a first call without a timeout, with a bad timeout or a bad buffer: EFAULT");

	errno = 0;
	int ret = gethostuuid(uuid, &wait);
	check(ret == 0, "gethostuuid() succeeds");
	if (ret != 0)
		printf("# errno %d (%s)\n", errno, strerror(errno));

	const char* source = expected_uuid(expected);
	snprintf(text, sizeof(text), "the UUID is %s", source ? source : "(no source found)");
	check(ret == 0 && source != NULL && uuid_compare(uuid, expected) == 0, text);
	check(ret == 0 && (uuid[6] >> 4) == 4 && (uuid[8] & 0xc0) == 0x80, "and a version 4 UUID");

	check(gethostuuid(again, &wait) == 0 && uuid_compare(uuid, again) == 0, "a second call gets the same UUID");
	check(run_self(argv[0], "print", text, sizeof(text)) == 0 && uuid_parse(text, other) == 0
	    && uuid_compare(uuid, other) == 0, "another process gets the same UUID");

	return failures;
}
