// fsgetpath() finds an object by its volume and file ID, as XNU's does, and getattrlist() says so (VOL_CAP_FMT_PATH_FROM_ID).
// Darling's used to fail with ENOTSUP and leave PATH_FROM_ID off: CFURL then had no _kCFURLFileIDKey for any file,
// LaunchServices registered every app as a placeholder with no SDK version, and SpringBoard ran each in a 375x667
// compatibility mode.
//
// The objects are made on $FSGETPATH_TEST_DIR, or else on Linux's /root when it is a tmpfs (as in a taffy container;
// otherwise the test skips them: a search of the host's root disk would read every directory on it). The prefix, an
// overlay whose files report its layers' devices, must not claim PATH_FROM_ID.
//
// With FSGETPATH_CONTRACT set, only what a program on $FSGETPATH_TEST_DIR's volume relies on: the volume claims
// PATH_FROM_ID, and fsgetpath() finds a file there from IDs another process got, before and after a lookup and after
// a move. Not XNU's errors and limits, nor what other volumes claim: Darling keeps those.
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
#include <time.h>
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

// the first regular file getattrlistbulk() lists in dir whose device isn't the directory's (an overlay's lower file),
// with the volume and file IDs it handed out: 0, or -1
static int bulk_file(const char* dir, char* path, fsid_t* fsid, uint64_t* fileid)
{
	struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT,
		.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_FSID | ATTR_CMN_FILEID };
	static char buf[65536];
	struct stat dir_st, st;
	int fd = open(dir, O_RDONLY | O_DIRECTORY), found = -1, n;

	if (fd < 0 || lstat(dir, &dir_st) != 0)
		return -1;
	while (found < 0 && (n = getattrlistbulk(fd, &al, buf, sizeof(buf), 0)) > 0)
	{
		char* e = buf;
		for (int i = 0; i < n && found < 0; i++)
		{
			uint32_t length;
			attrreference_t name;
			memcpy(&length, e, sizeof(length));
			// length, attribute_set_t, then the name's reference, the fsid and the file ID, packed
			char* p = e + sizeof(uint32_t) + sizeof(attribute_set_t);
			memcpy(&name, p, sizeof(name));
			memcpy(fsid, p + sizeof(name), sizeof(*fsid));
			memcpy(fileid, p + sizeof(name) + sizeof(*fsid), sizeof(*fileid));
			snprintf(path, PATH_MAX, "%s/%s", dir, p + name.attr_dataoffset);
			if (lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_dev != dir_st.st_dev && st.st_ino == *fileid)
				found = 0;
			e += length;
		}
	}
	close(fd);
	return found;
}

// CarbonCore's FSRef (FSPathMakeRef(), FSRefMakePath()), libsystem_kernel's: an 80-byte reference to an object
extern int sys_name_to_handle(const char* name, void* ref, int follow);
extern int sys_handle_to_name(void* ref, char name[4096]);

// a reference made by another process, which never looked the object up
static int ref_elsewhere(const char* path, unsigned char ref[80])
{
	struct
	{
		int ret;
		unsigned char ref[80];
	} b = { -1 };
	int fds[2];

	if (pipe(fds) != 0)
		return -1;
	pid_t pid = fork();
	if (pid == 0)
	{
		close(fds[0]);
		b.ret = sys_name_to_handle(path, b.ref, 0);
		write(fds[1], &b, sizeof(b));
		_exit(0);
	}
	close(fds[1]);
	ssize_t n = read(fds[0], &b, sizeof(b));
	close(fds[0]);
	waitpid(pid, NULL, 0);
	if (n != sizeof(b) || b.ret != 0)
		return -1;
	memcpy(ref, b.ref, sizeof(b.ref));
	return 0;
}

// whether the reference resolves to path
static int ref_names(unsigned char ref[80], const char* path)
{
	static char got[4096];
	int ret = sys_handle_to_name(ref, got);
	if (ret != 0 || strcmp(got, path) != 0)
	{
		printf("# FSRef: %s (want %s)\n", ret != 0 ? strerror(-ret) : got, path);
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
	const char* contract = getenv("FSGETPATH_CONTRACT");

	if (!contract)
		check(path_from_id("/private/tmp") == 0, "the prefix, an overlay whose files report its layers' devices, doesn't claim PATH_FROM_ID");

	// an overlay whose files report its own device (its layers on one file system, as Docker's): claims, and a file
	// there is found by searching
	if (!contract)
	{
		struct stat dir_st, file_st;
		const char* d = "/Volumes/SystemRoot/etc", *f = "/Volumes/SystemRoot/etc/passwd";
		if (statfs(d, &fs) == 0 && strcmp(fs.f_fstypename, "overlay") == 0 && lstat(d, &dir_st) == 0 &&
			lstat(f, &file_st) == 0)
		{
			if (file_st.st_dev == dir_st.st_dev)
				check(path_from_id(d) == 1 && ids_elsewhere(f, &fsid, &fileid) == 0 && finds(fsid, fileid, f),
					"Linux's root, an overlay whose files report its device, claims PATH_FROM_ID: a file there by searching");
			else
				check(path_from_id(d) == 0, "Linux's root, an overlay whose files report its layers' devices, doesn't claim PATH_FROM_ID");
		}
	}

	// getattrlistbulk() hands out IDs (CFURL's enumerators, LaunchServices' bundle scans), which fsgetpath() then finds:
	// for a file of the prefix's lower layer, whose device no mount is of, only by that
	if (!contract)
	{
		char path[PATH_MAX];
		if (path_from_id("/usr/lib/system") == 0)
			check(bulk_file("/usr/lib/system", path, &fsid, &fileid) == 0 && finds(fsid, fileid, path),
				"a file getattrlistbulk() listed, on a volume no search finds it on");
	}

	// a search of the default directory's volume must be cheap: Linux's /root a tmpfs, as in a taffy container, not the
	// host's root file system under a plain Darling (whose search would read every directory on the disk)
	if (contract && !getenv("FSGETPATH_TEST_DIR"))
	{
		check(0, "FSGETPATH_CONTRACT names a volume to check: set FSGETPATH_TEST_DIR");
		printf("# %d failed\n", failures);
		return failures;
	}
	if (!getenv("FSGETPATH_TEST_DIR") && (statfs(base, &fs) != 0 || strcmp(fs.f_fstypename, "tmpfs") != 0))
	{
		printf("ok - # SKIP %s isn't a tmpfs: set FSGETPATH_TEST_DIR to a directory on a small volume\n", base);
		printf("# %d failed\n", failures);
		return failures;
	}

	snprintf(dir, sizeof(dir), "%s/fsgetpath.XXXXXX", base);
	if (!mkdtemp(dir) || !realpath(dir, real) || statfs(real, &fs) != 0 || strcmp(fs.f_fstypename, "overlay") == 0)
	{
		check(0, "a directory to test in, on a volume that isn't an overlay (set FSGETPATH_TEST_DIR)");
		printf("# %d failed\n", failures);
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
	// symlinks the search must not follow, beside a/b: a loop, and a second way into b. Made after b, as tmpfs lists a
	// directory's newest entries first, so that the search meets them before b.
	char loop[PATH_MAX], dirlink[PATH_MAX];
	snprintf(loop, sizeof(loop), "%s/a/loop", real);
	snprintf(dirlink, sizeof(dirlink), "%s/a/dirlink", real);
	symlink(".", loop);
	symlink("b", dirlink);
	close(open(file, O_CREAT | O_WRONLY, 0644));

	if (path_from_id(real) != 1)
		printf("# %s is on %s, mounted at %s: it doesn't claim PATH_FROM_ID\n", real, fs.f_fstypename, fs.f_mntonname);
	check(path_from_id(real) == 1, "a volume that isn't an overlay claims PATH_FROM_ID");

	// object ID 2: the volume's root, as XNU's VFS_ROOT(), unless the Linux file system numbers another object 2 (tmpfs
	// numbers its root 1, and its first object 2)
	check(ids(real, &fsid, &fileid) == 0, "getattrlist() hands out the volume and file IDs");
	if (!contract)
	{
		char got[PATH_MAX];
		struct stat st, root_st;
		ssize_t n = fsgetpath(got, sizeof(got), &fsid, 2);
		int ok = n > 0 && lstat(got, &st) == 0 && lstat(fs.f_mntonname, &root_st) == 0 &&
			(st.st_ino == 2 || (strcmp(got, fs.f_mntonname) == 0 && root_st.st_ino != 2));
		if (!ok)
			printf("# ID 2: %s (the volume's root: %s)\n", n > 0 ? got : strerror(errno), fs.f_mntonname);
		check(ok, "object ID 2 is the object numbered 2, or else the volume's root (of its own mount)");
	}

	// a nested file this process never looked up: found by searching
	// (not by a/dirlink/file, nor through a/loop: finds() wants the path exactly)
	check(ids_elsewhere(file, &fsid, &fileid) == 0 && finds(fsid, fileid, file), "a nested file, by searching");
	// the fsid's second word only hints at the mount (binds of one file system share the first)
	if (!contract)
	{
		fsid_t first = fsid;
		first.val[1] = 0;
		check(finds(first, fileid, file), "the nested file, by the volume's first word alone");
	}
	// and once looked up: by that lookup
	check(ids(file, &fsid, &fileid) == 0 && finds(fsid, fileid, file), "a nested file, after a lookup");

	// a remembered lookup that no longer leads to the object is searched past
	rename(file, moved);
	check(finds(fsid, fileid, moved), "a moved file, at its new path");

	// CarbonCore's FSRef holds the object's volume and ID, which fsgetpath()'s lookup resolves: made by another process,
	// or here and then moved; and one to a file of the prefix, which only the path it was made from leads to
	if (!contract)
	{
		unsigned char ref[80];
		char refd[PATH_MAX], refm[PATH_MAX];
		snprintf(refd, sizeof(refd), "%s/a/b/ref", real);
		snprintf(refm, sizeof(refm), "%s/a/ref.moved", real);
		close(open(refd, O_CREAT | O_WRONLY, 0644));
		check(ref_elsewhere(refd, ref) == 0 && ref_names(ref, refd), "an FSRef another process made");
		check(sys_name_to_handle(refd, ref, 0) == 0 && rename(refd, refm) == 0 && ref_names(ref, refm),
			"an FSRef made here, to a file since moved");
		check(sys_name_to_handle("/usr/lib/libSystem.B.dylib", ref, 0) == 0 && ref_names(ref, "/usr/lib/libSystem.B.dylib"),
			"an FSRef to a file of the prefix");
		unlink(refm);
	}

	// a hard link: either of its names
	if (!contract)
	{
		char got[PATH_MAX];
		check(link(moved, link_) == 0, "a hard link");
		check(ids_elsewhere(link_, &fsid, &fileid) == 0, "a hard link's IDs");
		ssize_t n = fsgetpath(got, sizeof(got), &fsid, fileid);
		check(n > 0 && (strcmp(got, moved) == 0 || strcmp(got, link_) == 0), "a hard link, by one of its names");
	}

	// the deepest path a process can make, which it sees longer still: an answer, or ENOSPC, and never more than the
	// buffer. darling-xnu once wrote past its own on the stack, and this process died on the first fsgetpath() here.
	if (!contract)
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
	if (!contract)
	{
		char small[8];
		check(ids(moved, &fsid, &fileid) == 0, "IDs for the errors");
		check(fails_with(fsgetpath(small, sizeof(small), &fsid, fileid), ENOSPC), "a buffer the path doesn't fit: ENOSPC");
		char big[8192];
		// the length counts the NUL: a buffer of the path alone is ENOSPC, and nothing is written past it
		size_t n = strlen(moved) + 1;
		memset(big, 'x', sizeof(big));
		check(fails_with(fsgetpath(big, n - 1, &fsid, fileid), ENOSPC) && big[n - 1] == 'x',
			"a buffer of the path without its NUL: ENOSPC, nothing written past it");
		check(fsgetpath(big, n, &fsid, fileid) == (ssize_t) n && strcmp(big, moved) == 0, "a buffer of the path and its NUL");
		check(fails_with(fsgetpath(big, 0, &fsid, fileid), EINVAL), "a buffer of 0: EINVAL");
		check(fails_with(fsgetpath(big, sizeof(big) + 1, &fsid, fileid), EINVAL),
			"a buffer of more than MAXLONGPATHLEN (8192): EINVAL");
		check(fsgetpath(big, sizeof(big), &fsid, fileid) > 0, "a buffer of MAXLONGPATHLEN");
		check(fails_with(fsgetpath_ext(big, PATH_MAX, &fsid, fileid, 0x1), EINVAL), "an unknown option: EINVAL");
		check(fsgetpath_ext(big, PATH_MAX, &fsid, fileid, FSOPT_NOFIRMLINKPATH) > 0, "fsgetpath_ext(FSOPT_NOFIRMLINKPATH)");

		// held open until the checks are done, so that its inode number can't be taken by another file meanwhile (ext4
		// reuses them), and ENOENT stays the answer
		int gone_fd = open(gone, O_CREAT | O_WRONLY, 0644);
		check(gone_fd >= 0 && ids(gone, &fsid, &fileid) == 0, "a removed file's IDs");
		unlink(gone);
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		check(fails_with(fsgetpath(big, PATH_MAX, &fsid, fileid), ENOENT), "a removed file: ENOENT");
		clock_gettime(CLOCK_MONOTONIC, &t1);
		// what the volume's search costs when it can't be skipped (on ext4 as root, open_by_handle_at() skips it)
		printf("# ENOENT in %ld ms\n", (long) ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
		check(fails_with(fsgetpath(big, PATH_MAX, &fsid, fileid), ENOENT), "the removed file again: ENOENT");
		close(gone_fd);

		fsid_t none = { { 0x7ffffff0, 0 } };
		check(fails_with(fsgetpath(big, PATH_MAX, &none, 2), ENOTSUP), "a volume ID no volume has: ENOTSUP");
	}

	unlink(moved);
	unlink(link_);
	unlink(loop);
	unlink(dirlink);
	rmdir(sub);
	snprintf(sub, sizeof(sub), "%s/a", real);
	rmdir(sub);
	rmdir(real);
	printf("# %d failed\n", failures);
	return failures;
}
