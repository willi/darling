// Extended attributes with XNU's names, which have no namespace. Linux refuses those (EOPNOTSUPP), so installd could
// not mark an app it installs (com.apple.install_uuid) and no app could be installed.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xattr.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

int main(void)
{
	const char* base = getenv("TMPDIR") ? getenv("TMPDIR") : ".";
	char file[PATH_MAX], buf[256];
	const char* name = "com.apple.install_uuid";

	snprintf(file, sizeof(file), "%s/xattr.XXXXXX", base);
	int fd = mkstemp(file);
	if (fd < 0)
	{
		perror("mkstemp");
		return 1;
	}

	check(fsetxattr(fd, name, "1234", 4, 0, 0) == 0, "an attribute is set by file descriptor");
	check(getxattr(file, name, NULL, 0, 0, 0) == 4, "without a buffer, getxattr() gives the value's size");
	check(getxattr(file, name, buf, sizeof(buf), 0, 0) == 4 && memcmp(buf, "1234", 4) == 0,
		"getxattr() gives the value");

	ssize_t len = listxattr(file, buf, sizeof(buf), 0);
	check(len == (ssize_t) strlen(name) + 1 && strcmp(buf, name) == 0, "listxattr() gives the name as it was set");

	errno = 0;
	check(setxattr(file, name, "5", 1, 0, XATTR_CREATE) == -1 && errno == EEXIST,
		"XATTR_CREATE refuses an attribute that exists");

	check(removexattr(file, name, 0) == 0, "an attribute is removed");
	errno = 0;
	check(getxattr(file, name, buf, sizeof(buf), 0, 0) == -1 && errno == ENOATTR,
		"a missing attribute is ENOATTR");

	close(fd);
	unlink(file);
	return failures;
}
