// getattrlistbulk(): every entry of a directory exactly once, across calls whose buffer holds only a few entries, with
// the attributes of each. MobileInstallation enumerates the iOS Simulator runtime's apps this way; Darling's used to
// fail with ENOTSUP, so lsd registered no app and SpringBoard found none. It only relies on XNU's interface, so it
// runs on macOS too.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <unistd.h>

#define FILE_COUNT 40

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

int main(void)
{
	char dir[] = "/tmp/getattrlistbulk.XXXXXX";
	char path[1100];
	int seen[FILE_COUNT] = { 0 };
	int dups = 0, others = 0, sub_ok = 0, sizes_ok = 1, calls = 0;
	struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT,
		.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_OBJTYPE | ATTR_CMN_FILEID,
		.fileattr = ATTR_FILE_DATALENGTH };
	char buf[300]; // a few entries per call
	int fd;
	int n;

	if (!mkdtemp(dir))
		return 1;
	for (int i = 0; i < FILE_COUNT; i++)
	{
		snprintf(path, sizeof(path), "%s/file%02d", dir, i);
		int f = open(path, O_CREAT | O_WRONLY, 0644);
		write(f, "xxxxxxxxxx", i % 10); // file N has N % 10 bytes
		close(f);
	}
	snprintf(path, sizeof(path), "%s/sub", dir);
	mkdir(path, 0755);

	fd = open(dir, O_RDONLY | O_DIRECTORY);
	while ((n = getattrlistbulk(fd, &al, buf, sizeof(buf), 0)) > 0)
	{
		char* entry = buf;
		calls++;
		for (int i = 0; i < n; i++)
		{
			char* p = entry;
			uint32_t length = *(uint32_t*) p;
			attribute_set_t returned;
			attrreference_t name_ref;
			const char* name;
			fsobj_type_t type;
			off_t data_length = -1;

			p += sizeof(uint32_t);
			memcpy(&returned, p, sizeof(returned));
			p += sizeof(returned);
			memcpy(&name_ref, p, sizeof(name_ref));
			name = p + name_ref.attr_dataoffset;
			p += sizeof(name_ref);
			memcpy(&type, p, sizeof(type));
			p += sizeof(type);
			p += sizeof(uint64_t); // ATTR_CMN_FILEID
			if (returned.fileattr & ATTR_FILE_DATALENGTH)
				memcpy(&data_length, p, sizeof(data_length));

			int idx;
			if (sscanf(name, "file%d", &idx) == 1 && idx >= 0 && idx < FILE_COUNT)
			{
				if (seen[idx]++)
					dups++;
				if (type != VREG || data_length != idx % 10)
					sizes_ok = 0;
			}
			else if (strcmp(name, "sub") == 0)
				// a directory: no file attributes
				sub_ok = type == VDIR && !(returned.fileattr & ATTR_FILE_DATALENGTH);
			else
				others++;
			entry += length;
		}
	}

	int missing = 0;
	for (int i = 0; i < FILE_COUNT; i++)
		missing += !seen[i];
	check(n == 0, "enumeration ends with 0");
	check(calls > 1, "a small buffer takes several calls");
	check(!missing && !dups && !others, "every entry once, without . and ..");
	check(sizes_ok, "files report their type and data length");
	check(sub_ok, "a directory reports VDIR and no file attributes");

	lseek(fd, 0, SEEK_SET);
	errno = 0;
	check(getattrlistbulk(fd, &al, buf, 8, 0) == -1, "a buffer too small for one entry fails");

	struct attrlist no_returned = al;
	no_returned.commonattr &= ~ATTR_CMN_RETURNED_ATTRS;
	errno = 0;
	check(getattrlistbulk(fd, &no_returned, buf, sizeof(buf), 0) == -1 && errno == EINVAL,
		"ATTR_CMN_RETURNED_ATTRS is required");
	close(fd);

	for (int i = 0; i < FILE_COUNT; i++)
	{
		snprintf(path, sizeof(path), "%s/file%02d", dir, i);
		unlink(path);
	}
	snprintf(path, sizeof(path), "%s/sub", dir);
	rmdir(path);
	rmdir(dir);
	return failures;
}
