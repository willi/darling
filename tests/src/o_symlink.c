// open() with O_SYMLINK opens a symlink itself, and anything else as without it, as XNU does. Darling's opened every
// object as a Linux O_PATH descriptor, which can't be read or listed, so MobileInstallation couldn't stage an app
// (getattrlistbulk() on the directory it opened failed with EBADF) and no app could be installed.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/stat.h>
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
	char dir[PATH_MAX], file[PATH_MAX], link[PATH_MAX];
	char buf[256];
	struct stat st;

	snprintf(dir, sizeof(dir), "%s/o_symlink.XXXXXX", base);
	if (!mkdtemp(dir))
	{
		perror("mkdtemp");
		return 1;
	}
	snprintf(file, sizeof(file), "%s/data", dir);
	snprintf(link, sizeof(link), "%s/link", dir);
	int fd = open(file, O_CREAT | O_WRONLY, 0644);
	write(fd, "hello", 5);
	close(fd);
	symlink("data", link);

	// a directory: listed, as MobileInstallation's staging does
	{
		struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT,
			.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME };
		char attrs[4096];
		fd = open(dir, O_RDONLY | O_SYMLINK);
		check(fd >= 0 && getattrlistbulk(fd, &al, attrs, sizeof(attrs), 0) == 2,
			"a directory opened with O_SYMLINK can be listed");
		close(fd);
	}

	// a file: read
	fd = open(file, O_RDONLY | O_SYMLINK);
	check(fd >= 0 && read(fd, buf, sizeof(buf)) == 5, "a file opened with O_SYMLINK can be read");
	close(fd);

	// a symlink: the link itself, not what it points to
	fd = open(link, O_RDONLY | O_SYMLINK);
	check(fd >= 0 && fstat(fd, &st) == 0 && S_ISLNK(st.st_mode), "a symlink opened with O_SYMLINK is the link itself");
	close(fd);

	unlink(link);
	unlink(file);
	rmdir(dir);
	return failures;
}
