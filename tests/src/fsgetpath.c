// fsgetpath() finds an object by its volume and file ID, as XNU's does, and getattrlist() says so (VOL_CAP_FMT_PATH_FROM_ID).
// Darling's used to fail with ENOTSUP and leave PATH_FROM_ID off: CFURL then had no _kCFURLFileIDKey for any file,
// LaunchServices registered every app as a placeholder with no SDK version, and SpringBoard ran each in a 375x667
// compatibility mode.
//
// The objects are made on a volume that isn't an overlay (fsgetpath() can't search an overlay, whose files are on its
// layers' devices): $FSGETPATH_TEST_DIR, or Linux's /root, a tmpfs in a taffy container. The prefix itself is an
// overlay, which must not claim PATH_FROM_ID.
//
// Prints one "ok"/"not ok" line per check, then "# N failed" (a run without that line crashed); the exit status is the
// number of failed checks.

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/fsgetpath.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern ssize_t fsgetpath_ext(char*, size_t, fsid_t*, uint64_t, uint32_t);

#ifndef FSOPT_NOFIRMLINKPATH
#	define FSOPT_NOFIRMLINKPATH 0x00000080
#endif

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

struct caps_buf
{
	uint32_t length;
	vol_capabilities_attr_t caps;
} __attribute__((packed));

// whether the volume path is on says it can find a path from an ID (-1: getattrlist() failed)
static int path_from_id(const char* path)
{
	struct statfs fs;
	struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT, .volattr = ATTR_VOL_INFO | ATTR_VOL_CAPABILITIES };
	struct caps_buf b;

	if (statfs(path, &fs) != 0 || getattrlist(fs.f_mntonname, &al, &b, sizeof(b), 0) != 0)
		return -1;
	uint32_t f = b.caps.capabilities[VOL_CAPABILITIES_FORMAT], v = b.caps.valid[VOL_CAPABILITIES_FORMAT];
	return (v & VOL_CAP_FMT_PATH_FROM_ID) && (f & VOL_CAP_FMT_PATH_FROM_ID);
}

struct id_buf
{
	uint32_t length;
	fsid_t fsid;
	uint64_t fileid;
} __attribute__((packed));

// the object's volume and file ID, as getattrlist() hands them out
static int ids(const char* path, fsid_t* fsid, uint64_t* fileid)
{
	struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT, .commonattr = ATTR_CMN_FSID | ATTR_CMN_FILEID };
	struct id_buf b;

	if (getattrlist(path, &al, &b, sizeof(b), FSOPT_NOFOLLOW) != 0)
		return -1;
	*fsid = b.fsid;
	*fileid = b.fileid;
	return 0;
}

// the same, of an open file
static int fd_ids(int fd, fsid_t* fsid, uint64_t* fileid)
{
	struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT, .commonattr = ATTR_CMN_FSID | ATTR_CMN_FILEID };
	struct id_buf b;

	if (fgetattrlist(fd, &al, &b, sizeof(b), 0) != 0)
		return -1;
	*fsid = b.fsid;
	*fileid = b.fileid;
	return 0;
}

// the same, looked up by another process (by path, or with no path, by fd): this one never looked the object up, so
// fsgetpath() has to search
static int ids_elsewhere_of(const char* path, int fd, fsid_t* fsid, uint64_t* fileid)
{
	int fds[2];
	struct id_buf b = { 0 };

	if (pipe(fds) != 0)
		return -1;
	pid_t pid = fork();
	if (pid == 0)
	{
		fsid_t f;
		uint64_t id;
		close(fds[0]);
		b.length = (path ? ids(path, &f, &id) : fd_ids(fd, &f, &id)) == 0;
		b.fsid = f;
		b.fileid = id;
		write(fds[1], &b, sizeof(b));
		_exit(0);
	}
	close(fds[1]);
	ssize_t n = read(fds[0], &b, sizeof(b));
	close(fds[0]);
	waitpid(pid, NULL, 0);
	if (n != sizeof(b) || !b.length)
		return -1;
	*fsid = b.fsid;
	*fileid = b.fileid;
	return 0;
}

static int ids_elsewhere(const char* path, fsid_t* fsid, uint64_t* fileid)
{
	return ids_elsewhere_of(path, -1, fsid, fileid);
}

// whether fsgetpath() names path, and returns the length of what it wrote, its NUL counted
static int finds(fsid_t fsid, uint64_t fileid, const char* path)
{
	char got[PATH_MAX];
	memset(got, 'x', sizeof(got));
	ssize_t n = fsgetpath(got, sizeof(got), &fsid, fileid);
	if (n < 0)
	{
		printf("# fsgetpath: %s (want %s)\n", strerror(errno), path);
		return 0;
	}
	if (n != (ssize_t) strlen(path) + 1 || strcmp(got, path) != 0)
	{
		printf("# fsgetpath: %zd \"%.*s\" (want %s)\n", n, (int) (n > 0 ? n : 0), got, path);
		return 0;
	}
	return 1;
}

static int fails_with(ssize_t n, int err)
{
	if (n >= 0 || errno != err)
	{
		printf("# got %zd, errno %d (want errno %d)\n", n, n < 0 ? errno : 0, err);
		return 0;
	}
	return 1;
}

// The deepest directory a process can make: a Linux path of LINUX_PATH_MAX - 2 bytes (Linux's PATH_MAX, not Darwin's
// 1024; Darling's path expansion keeps a byte for a trailing "/"), in levels made relative to the one above, as a
// process can't name the path whole. Outside the prefix the process sees it 19 bytes longer, under
// /Volumes/SystemRoot, which is past a page. want gets that path; fds[] the directories' descriptors, the deepest last.
#define LINUX_PATH_MAX 4096
#define DEEP_LEVELS 32
struct deep
{
	char want[LINUX_PATH_MAX + 64], names[DEEP_LEVELS][256];
	int fds[DEEP_LEVELS + 1], n;
};

static int make_deep(const char* real, struct deep* d)
{
	const char* exit_path = "/Volumes/SystemRoot";
	if (strncmp(real, exit_path, strlen(exit_path)) != 0)
		return -1;
	int left = (LINUX_PATH_MAX - 2) - (int) (strlen(real) - strlen(exit_path));
	int at = open(real, O_RDONLY | O_DIRECTORY);
	strcpy(d->want, real);
	d->n = 0;
	while (at >= 0 && left > 0 && d->n < DEEP_LEVELS)
	{
		// "/" and a name of up to 200 bytes; never a last level of just "/"
		int len = left - 1 > 200 ? 200 : left - 1;
		if (left - (1 + len) == 1)
			len--;
		memset(d->names[d->n], 'd', len);
		d->names[d->n][len] = '\0';
		strcat(d->want, "/");
		strcat(d->want, d->names[d->n]);
		left -= 1 + len;
		int fd = mkdirat(at, d->names[d->n], 0755) == 0 ? openat(at, d->names[d->n], O_RDONLY | O_DIRECTORY) : -1;
		d->fds[d->n++] = at;
		at = fd;
	}
	d->fds[d->n] = at;
	if (at < 0 || left != 0)
		printf("# the deep tree stopped %d bytes short, at level %d: %s\n", left, d->n, strerror(errno));
	return at >= 0 && left == 0 ? 0 : -1;
}

static void remove_deep(struct deep* d)
{
	for (int i = d->n - 1; i >= 0; i--)
	{
		close(d->fds[i + 1]);
		unlinkat(d->fds[i], d->names[i], AT_REMOVEDIR);
	}
	close(d->fds[0]);
}

int main(void)
{
	// each line as it's checked, so a crash shows where
	setvbuf(stdout, NULL, _IOLBF, 0);
	const char* base = getenv("FSGETPATH_TEST_DIR") ? getenv("FSGETPATH_TEST_DIR") : "/Volumes/SystemRoot/root";
	char dir[PATH_MAX], real[PATH_MAX], sub[PATH_MAX], file[PATH_MAX], link_[PATH_MAX], moved[PATH_MAX], gone[PATH_MAX];
	struct statfs fs;
	fsid_t fsid;
	uint64_t fileid;

	check(path_from_id("/private/tmp") == 0, "the prefix, an overlay, doesn't claim PATH_FROM_ID");

	snprintf(dir, sizeof(dir), "%s/fsgetpath.XXXXXX", base);
	if (!mkdtemp(dir) || !realpath(dir, real) || statfs(real, &fs) != 0 || strcmp(fs.f_fstypename, "overlay") == 0)
	{
		check(0, "a directory to test in, on a volume that isn't an overlay (set FSGETPATH_TEST_DIR)");
		return failures;
	}
	snprintf(file, sizeof(file), "%s/a/b/file", real);
	snprintf(link_, sizeof(link_), "%s/link", real);
	snprintf(moved, sizeof(moved), "%s/a/moved", real);
	snprintf(gone, sizeof(gone), "%s/gone", real);
	snprintf(sub, sizeof(sub), "%s/a", real);
	mkdir(sub, 0755);
	snprintf(sub, sizeof(sub), "%s/a/b", real);
	mkdir(sub, 0755);
	close(open(file, O_CREAT | O_WRONLY, 0644));

	check(path_from_id(real) == 1, "a volume that isn't an overlay claims PATH_FROM_ID");

	// the volume's root: object ID 2, as XNU's VFS_ROOT()
	check(ids(real, &fsid, &fileid) == 0, "getattrlist() hands out the volume and file IDs");
	check(finds(fsid, 2, fs.f_mntonname), "object ID 2 is the volume's root");

	// a nested file this process never looked up: found by searching
	check(ids_elsewhere(file, &fsid, &fileid) == 0 && finds(fsid, fileid, file), "a nested file, by searching");
	// and once looked up: by that lookup
	check(ids(file, &fsid, &fileid) == 0 && finds(fsid, fileid, file), "a nested file, after a lookup");

	// a remembered lookup that no longer leads to the object is searched past
	rename(file, moved);
	check(finds(fsid, fileid, moved), "a moved file, at its new path");

	// a hard link: either of its names
	{
		char got[PATH_MAX];
		check(link(moved, link_) == 0, "a hard link");
		check(ids_elsewhere(link_, &fsid, &fileid) == 0, "a hard link's IDs");
		ssize_t n = fsgetpath(got, sizeof(got), &fsid, fileid);
		check(n > 0 && (strcmp(got, moved) == 0 || strcmp(got, link_) == 0), "a hard link, by one of its names");
	}

	// the deepest path a process can make, which it sees longer still: an answer, or ENOSPC, and never more than the
	// buffer. darling-xnu once wrote past its own on the stack, and this process died on the first fsgetpath() here.
	{
		static struct deep d;
		char got[LINUX_PATH_MAX];
		check(make_deep(real, &d) == 0 && ids_elsewhere_of(0, d.fds[d.n], &fsid, &fileid) == 0,
			"a directory at the deepest path a process can make");
		// XNU takes buffers of up to MAXLONGPATHLEN, 8192
		static char big[8192];
		size_t want = strlen(d.want) + 1;
		check(fails_with(fsgetpath(got, sizeof(got), &fsid, fileid), ENOSPC),
			"a path longer than a page, as the process sees it, in a page: ENOSPC");
		check(fsgetpath(big, sizeof(big), &fsid, fileid) == (ssize_t) want && strcmp(big, d.want) == 0,
			"the same path, in a buffer of 8192");
		memset(big, 'x', sizeof(big));
		check(fails_with(fsgetpath(big, want - 1, &fsid, fileid), ENOSPC) && big[want - 2] == 'x',
			"a buffer one byte short of the path and its NUL: ENOSPC, nothing written");
		check(fsgetpath(big, want, &fsid, fileid) == (ssize_t) want && big[want - 1] == '\0',
			"a buffer of exactly the path and its NUL");
		remove_deep(&d);
	}

	// errors, in XNU's order
	{
		char small[8];
		check(ids(moved, &fsid, &fileid) == 0, "IDs for the errors");
		check(fails_with(fsgetpath(small, sizeof(small), &fsid, fileid), ENOSPC), "a buffer the path doesn't fit: ENOSPC");
		char big[8192];
		check(fails_with(fsgetpath(big, 0, &fsid, fileid), EINVAL), "a buffer of 0: EINVAL");
		check(fails_with(fsgetpath(big, sizeof(big) + 1, &fsid, fileid), EINVAL),
			"a buffer of more than MAXLONGPATHLEN (8192): EINVAL");
		check(fsgetpath(big, sizeof(big), &fsid, fileid) > 0, "a buffer of MAXLONGPATHLEN");
		check(fails_with(fsgetpath_ext(big, PATH_MAX, &fsid, fileid, 0x1), EINVAL), "an unknown option: EINVAL");
		check(fsgetpath_ext(big, PATH_MAX, &fsid, fileid, FSOPT_NOFIRMLINKPATH) > 0, "fsgetpath_ext(FSOPT_NOFIRMLINKPATH)");

		close(open(gone, O_CREAT | O_WRONLY, 0644));
		check(ids(gone, &fsid, &fileid) == 0, "a removed file's IDs");
		unlink(gone);
		check(fails_with(fsgetpath(big, PATH_MAX, &fsid, fileid), ENOENT), "a removed file: ENOENT");

		fsid_t none = { { 0x7ffffff0, 0 } };
		check(fails_with(fsgetpath(big, PATH_MAX, &none, 2), ENOTSUP), "a volume ID no volume has: ENOTSUP");
	}

	unlink(moved);
	unlink(link_);
	rmdir(sub);
	snprintf(sub, sizeof(sub), "%s/a", real);
	rmdir(sub);
	rmdir(real);
	printf("# %d failed\n", failures);
	return failures;
}
