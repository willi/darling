// getattrlist() and setattrlist() accept every attribute XNU knows, as XNU does. Darling's used to fail with EINVAL
// for any attribute outside a few, so CFURL could not list a directory with resource values (contentsOfDirectoryAtURL:
// includingPropertiesForKeys:), lsd lost its database across a restart, and SpringBoard found no app.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

static int is_zero(const void* p, size_t n)
{
	const unsigned char* c = p;
	for (size_t i = 0; i < n; i++)
		if (c[i])
			return 0;
	return 1;
}

int main(void)
{
	const char* base = getenv("TMPDIR") ? getenv("TMPDIR") : ".";
	char dir[PATH_MAX], file[PATH_MAX], real[PATH_MAX];
	char buf[2048];
	struct stat st;

	snprintf(dir, sizeof(dir), "%s/getattrlist.XXXXXX", base);
	if (!mkdtemp(dir))
	{
		perror("mkdtemp");
		return 1;
	}
	snprintf(file, sizeof(file), "%s/data", dir);
	int fd = open(file, O_CREAT | O_WRONLY, 0644);
	write(fd, "hello", 5);
	close(fd);
	stat(file, &st);

	// a file: what was asked for, in the order of the bits, all of it returned
	{
		struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT,
			.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_OBJTYPE | ATTR_CMN_MODTIME | ATTR_CMN_FILEID,
			.fileattr = ATTR_FILE_TOTALSIZE };
		struct __attribute__((packed))
		{
			uint32_t length;
			attribute_set_t returned;
			fsobj_type_t type;
			struct timespec mtime;
			uint64_t fileid;
			off_t size;
		} r;
		int rv = getattrlist(file, &al, &r, sizeof(r), 0);
		check(rv == 0 && r.length == sizeof(r) && r.returned.commonattr == al.commonattr &&
				r.returned.fileattr == al.fileattr,
			"a file's attributes are all returned");
		check(rv == 0 && r.type == VREG && r.mtime.tv_sec == st.st_mtime && r.fileid == st.st_ino && r.size == 5,
			"a file's type, modification time, number and size");
	}

	// a directory: its name and how many entries it has; file attributes don't apply and aren't returned
	{
		struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT,
			.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_OBJTYPE,
			.dirattr = ATTR_DIR_ENTRYCOUNT,
			.fileattr = ATTR_FILE_TOTALSIZE };
		struct __attribute__((packed))
		{
			uint32_t length;
			attribute_set_t returned;
			attrreference_t name;
			fsobj_type_t type;
			uint32_t entries;
		}* r = (void*) buf;
		int rv = getattrlist(dir, &al, buf, sizeof(buf), 0);
		const char* name = (const char*) &r->name + r->name.attr_dataoffset;
		check(rv == 0 && r->type == VDIR && r->entries == 1 && r->returned.fileattr == 0,
			"a directory's type and entry count, without file attributes");
		check(rv == 0 && strcmp(name, strrchr(dir, '/') + 1) == 0, "a directory's name");
	}

	// an attribute there is no answer for (a file's UUID): left out of what was returned, or zeros in its place
	{
		struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT,
			.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_OBJTYPE | ATTR_CMN_UUID | ATTR_CMN_FILEID };
		struct __attribute__((packed))
		{
			uint32_t length;
			attribute_set_t returned;
			fsobj_type_t type;
			uint64_t fileid;
		} r;
		int rv = getattrlist(file, &al, &r, sizeof(r), 0);
		check(rv == 0 && !(r.returned.commonattr & ATTR_CMN_UUID) && r.fileid == st.st_ino,
			"an attribute without an answer is left out when the caller asks what was returned");

		al.commonattr &= ~ATTR_CMN_RETURNED_ATTRS;
		struct __attribute__((packed))
		{
			uint32_t length;
			fsobj_type_t type;
			unsigned char uuid[16];
			uint64_t fileid;
		} z;
		rv = getattrlist(file, &al, &z, sizeof(z), 0);
		check(rv == 0 && z.length == sizeof(z) && is_zero(&z.uuid, sizeof(z.uuid)) && z.fileid == st.st_ino,
			"otherwise zeros take its place");
	}

	// the full path, through a file descriptor
	{
		struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT, .commonattr = ATTR_CMN_FULLPATH };
		struct __attribute__((packed))
		{
			uint32_t length;
			attrreference_t path;
		}* r = (void*) buf;
		fd = open(file, O_RDONLY);
		int rv = fgetattrlist(fd, &al, buf, sizeof(buf), 0);
		close(fd);
		realpath(file, real);
		check(rv == 0 && strcmp((const char*) &r->path + r->path.attr_dataoffset, real) == 0,
			"fgetattrlist() gives the full path");
	}

	// the volume a file is on: where it's mounted, and how big it is. CFURL looks a bundle's volume up this way, and
	// LaunchServices couldn't register CoreTypes without it.
	{
		struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT,
			.commonattr = ATTR_CMN_RETURNED_ATTRS,
			.volattr = ATTR_VOL_INFO | ATTR_VOL_SIZE | ATTR_VOL_MOUNTPOINT };
		struct __attribute__((packed))
		{
			uint32_t length;
			attribute_set_t returned;
			off_t size;
			attrreference_t mountpoint;
		}* r = (void*) buf;
		int rv = getattrlist(file, &al, buf, sizeof(buf), 0);
		const char* mnt = (const char*) &r->mountpoint + r->mountpoint.attr_dataoffset;
		check(rv == 0 && r->returned.volattr == (ATTR_VOL_SIZE | ATTR_VOL_MOUNTPOINT) && r->size > 0 && mnt[0] == '/',
			"a file's volume: its size and mount point");
	}

	// statfs() agrees: the mount point is a directory the process can see, and the file system's ID is the device
	// getattrlist() reports. Darling's used to give Linux's mount points, and CFURL put a bundle on /proc's volume.
	{
		struct statfs fs;
		struct stat mnt;
		struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT, .commonattr = ATTR_CMN_DEVID };
		struct __attribute__((packed))
		{
			uint32_t length;
			dev_t dev;
		} r;
		int rv = statfs(file, &fs);
		check(rv == 0 && stat(fs.f_mntonname, &mnt) == 0 && S_ISDIR(mnt.st_mode) && strncmp(real, fs.f_mntonname, strlen(fs.f_mntonname)) == 0,
			"statfs() gives a mount point the file is under");
		check(rv == 0 && getattrlist(file, &al, &r, sizeof(r), 0) == 0 && r.dev == fs.f_fsid.val[0] && r.dev == st.st_dev,
			"statfs()'s file system ID is the device getattrlist() and stat() report");
	}

	// a bit XNU doesn't know (the obsolete ATTR_FILE_FILETYPE)
	{
		struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT, .fileattr = 0x40 };
		errno = 0;
		check(getattrlist(file, &al, buf, sizeof(buf), 0) == -1 && errno == EINVAL, "an unknown attribute is refused");
	}

	// setattrlist(): the modification time is set; flags have nowhere to go but are accepted; the type can't be set
	{
		struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT, .commonattr = ATTR_CMN_MODTIME | ATTR_CMN_FLAGS };
		struct __attribute__((packed))
		{
			struct timespec mtime;
			uint32_t flags;
		} v = { { 1000000000, 0 }, 0 };
		struct stat after;
		int rv = setattrlist(file, &al, &v, sizeof(v), 0);
		stat(file, &after);
		check(rv == 0 && after.st_mtime == 1000000000, "setattrlist() sets the modification time");

		al.commonattr = ATTR_CMN_OBJTYPE;
		errno = 0;
		check(setattrlist(file, &al, &v, sizeof(v), 0) == -1 && errno == EINVAL,
			"setattrlist() refuses what can't be set");
	}

	unlink(file);
	rmdir(dir);
	return failures;
}
