// access_extended (accessx_np) checks a list of files in one call, and clonefileat/fclonefileat refuse with
// ENOTSUP on a volume without clones, as XNU answers them (xnu-12377.121.6 bsd/vfs/vfs_syscalls.c). Darling had
// no syscall 284, so accessx_np failed with ENOSYS: CoreServicesInternal's _URLReplaceObject calls it before
// every atomic replace, and lsd couldn't save its database, rebuilding it at every boot. Its clonefile stubs
// returned ENOSYS too, which Biome's bm_replace_file doesn't fall back on (it copies on ENOTSUP, retries another
// name on EEXIST).
//
// Darling checks permissions as the Linux process's identity, so the checks for someone else's files run in a
// child that switches its Linux uid (Darling's setuid() only changes the uid Darling reports), and the
// superuser's checks need Linux root: without it, both are skipped.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <copyfile.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CLONE_NOFOLLOW_ANY
#define CLONE_NOFOLLOW_ANY 0x0008
#endif
#ifndef CLONE_RESOLVE_BENEATH
#define CLONE_RESOLVE_BENEATH 0x0010
#endif
#ifndef ENOTCAPABLE
#define ENOTCAPABLE 107
#endif

// Linux's own calls, for what Darling deliberately doesn't change (the Linux ids, owners, file attributes)
extern int __linux_syscall(int nr, ...);
#if defined(__x86_64__)
#define LINUX_NR_ioctl 16
#define LINUX_NR_fchown 93
#define LINUX_NR_getuid 102
#define LINUX_NR_setgroups 116
#define LINUX_NR_setresuid 117
#define LINUX_NR_setresgid 119
#define LINUX_FS_IOC_SETFLAGS 0x40086602
#elif defined(__i386__)
#define LINUX_NR_ioctl 54
#define LINUX_NR_fchown 207 // fchown32
#define LINUX_NR_getuid 199 // getuid32
#define LINUX_NR_setgroups 206 // setgroups32
#define LINUX_NR_setresuid 208 // setresuid32
#define LINUX_NR_setresgid 210 // setresgid32
#define LINUX_FS_IOC_SETFLAGS 0x40046602
#elif defined(__aarch64__)
#define LINUX_NR_ioctl 29
#define LINUX_NR_fchown 55
#define LINUX_NR_getuid 174
#define LINUX_NR_setgroups 159
#define LINUX_NR_setresuid 147
#define LINUX_NR_setresgid 149
#define LINUX_FS_IOC_SETFLAGS 0x40086602
#else
#error "Linux system call numbers for this architecture"
#endif
#define LINUX_FS_IMMUTABLE_FL 0x10
#define LINUX_FS_APPEND_FL 0x20
#define NOBODY 65534

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	fflush(stdout);
	if (!ok)
		failures++;
}

static void skip(const char* what, const char* why)
{
	printf("ok - %s # SKIP %s\n", what, why);
	fflush(stdout);
}

// a request: up to 8 descriptors, then their names
struct request
{
	struct accessx_descriptor d[8];
	char names[4 * PATH_MAX];
	size_t n, used;
};

static void add(struct request* r, const char* name, int flags)
{
	r->d[r->n].ad_flags = flags;
	r->d[r->n].ad_pad[0] = r->d[r->n].ad_pad[1] = 0;
	if (!name)
		r->d[r->n].ad_name_offset = 0;
	else
	{
		// names follow the descriptors: lay_out() fixes the offsets up
		r->d[r->n].ad_name_offset = (unsigned int) r->used + 1;
		strcpy(r->names + r->used, name);
		r->used += strlen(name) + 1;
	}
	r->n++;
}

// lays the request out as the kernel reads it, in buf; returns its size
static size_t lay_out(struct request* r, char* buf)
{
	size_t table = r->n * sizeof(struct accessx_descriptor);
	for (size_t i = 0; i < r->n; i++)
	{
		struct accessx_descriptor d = r->d[i];
		if (d.ad_name_offset)
			d.ad_name_offset = (unsigned int) (table + d.ad_name_offset - 1);
		memcpy(buf + i * sizeof(d), &d, sizeof(d));
	}
	memcpy(buf + table, r->names, r->used);
	return table + r->used;
}

static int run(struct request* r, int* results)
{
	static char buf[sizeof(struct request)];
	size_t size = lay_out(r, buf);
	return accessx_np((const struct accessx_descriptor*) buf, size, results, getuid());
}

// one descriptor's answer, or -1 when the call failed
static int ask(const char* name, int flags)
{
	struct request r = { 0 };
	int res[1];
	add(&r, name, flags);
	return run(&r, res) == 0 ? res[0] : -1;
}

// an errno answer: got (a result, or -1 for "the call failed with errno") against want
static void check_errno(int got, int want, const char* what)
{
	check(got == want, what);
	if (got != want)
		printf("# got %d (%s), want %d (%s)\n", got, got > 0 ? strerror(got) : "-", want, want > 0 ? strerror(want) : "-");
}

// a call that should fail with want
static void check_fails(int ret, int want, const char* what)
{
	check_errno(ret == -1 ? errno : 0, want, what);
}

static void write_file(const char* path, mode_t mode)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
	write(fd, "hello", 5);
	close(fd);
	chmod(path, mode);
}

// Linux's file attributes (chattr), or -1 where the file system has none
static int set_linux_flags(const char* path, int flags)
{
	int fd = open(path, O_RDONLY);
	int ret = fd < 0 ? -1 : __linux_syscall(LINUX_NR_ioctl, fd, LINUX_FS_IOC_SETFLAGS, &flags);
	close(fd);
	return ret < 0 ? -1 : 0;
}

// the Linux owner of a file (Darling's chown() changes nothing)
static int set_linux_owner(const char* path, int uid)
{
	int fd = open(path, O_RDONLY);
	int ret = fd < 0 ? -1 : __linux_syscall(LINUX_NR_fchown, fd, uid, uid);
	close(fd);
	return ret;
}

// what holds whoever asks: same-path entries, missing files, the descriptor table's rules
static void anyone(const char* dir, const char* file, const char* missing)
{
	struct request r = { 0 };
	int res[8];

	// same-path entries: one lookup, three answers
	add(&r, file, R_OK);
	add(&r, NULL, _READ_OK | _RATTR_OK);
	add(&r, NULL, F_OK);
	// a missing file answers its own descriptors (and its offset-0 followers), not the call
	add(&r, missing, R_OK);
	add(&r, NULL, F_OK);
	add(&r, dir, X_OK);
	int ret = run(&r, res);
	check(ret == 0, "accessx_np succeeds");
	if (ret == 0)
	{
		check_errno(res[0], 0, "a 0644 file is readable");
		check_errno(res[1], 0, "a 0644 file's data and attributes are readable (same-path entry)");
		check_errno(res[2], 0, "a file exists (same-path entry)");
		check_errno(res[3], ENOENT, "a missing file is ENOENT");
		check_errno(res[4], ENOENT, "a missing file's offset-0 entry is ENOENT too");
		check_errno(res[5], 0, "search a 0755 directory");
	}
	else
		printf("# accessx_np: %s\n", strerror(errno));
}

static void malformed(const char* file, const char* missing)
{
	struct request r = { 0 };
	static char buf[32 * 1024];
	int res[128];
	size_t size;

	add(&r, file, R_OK);
	size = lay_out(&r, buf);

	errno = 0;
	check_fails(accessx_np((const struct accessx_descriptor*) buf, 16 * 1024 + 1, res, 0), ENOMEM,
		"a table over ACCESSX_MAX_TABLESIZE is ENOMEM");
	errno = 0;
	check_fails(accessx_np((const struct accessx_descriptor*) buf, sizeof(struct accessx_descriptor) + 1, res, 0), EINVAL,
		"a table without room for a name is EINVAL");

	struct accessx_descriptor* d = (struct accessx_descriptor*) buf;
	unsigned int good = d[0].ad_name_offset;
	d[0].ad_name_offset = 0;
	errno = 0;
	check_fails(accessx_np(d, size, res, 0), EINVAL, "a first descriptor without a name is EINVAL");
	// XNU checks the table before it copies the results out
	errno = 0;
	check_fails(accessx_np(d, size, NULL, 0), EINVAL, "a malformed table is EINVAL even with no results array");
	d[0].ad_name_offset = (unsigned int) size;
	errno = 0;
	check_fails(accessx_np(d, size, res, 0), EINVAL, "a name offset past the table is EINVAL");
	d[0].ad_name_offset = good;
	errno = 0;
	check_fails(accessx_np(d, size, NULL, 0), EFAULT, "a good table with no results array is EFAULT");

	// two descriptors, the second's name inside the first descriptor (an offset under one descriptor's
	// size is XNU's "the previous name", so this one points at the second descriptor itself)
	memset(&r, 0, sizeof(r));
	add(&r, file, R_OK);
	add(&r, file, R_OK);
	size = lay_out(&r, buf);
	d[1].ad_name_offset = sizeof(struct accessx_descriptor);
	errno = 0;
	check_fails(accessx_np(d, size, res, 0), EINVAL, "a name offset into the descriptors is EINVAL");

	// names laid out in the other order: the descriptors end where the lowest-addressed name begins, which is
	// the second descriptor's
	memset(buf, 0, sizeof(buf));
	d[0].ad_flags = d[1].ad_flags = R_OK;
	d[1].ad_name_offset = 2 * sizeof(struct accessx_descriptor);
	strcpy(buf + d[1].ad_name_offset, missing);
	d[0].ad_name_offset = (unsigned int) (d[1].ad_name_offset + strlen(missing) + 1);
	strcpy(buf + d[0].ad_name_offset, file);
	size = d[0].ad_name_offset + strlen(file) + 1;
	res[0] = res[1] = -1;
	int ret = accessx_np(d, size, res, 0);
	check(ret == 0 && res[0] == 0 && res[1] == ENOENT, "names in the other order: each descriptor gets its own name's answer");

	// a lookup error that isn't the name's (ENAMETOOLONG) fails the call, and writes no results
	memset(&r, 0, sizeof(r));
	char long_name[1100];
	memset(long_name, 'a', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';
	add(&r, file, R_OK);
	add(&r, long_name, R_OK);
	res[0] = res[1] = 12345;
	errno = 0;
	ret = run(&r, res);
	check_fails(ret, ENAMETOOLONG, "a name of MAXPATHLEN bytes or more fails the call: ENAMETOOLONG");
	check(res[0] == 12345, "a failed call writes no results");

	// ACCESSX_MAX_DESCRIPTORS for one name is allowed, one more isn't
	for (int n = 100; n <= 101; n++)
	{
		memset(buf, 0, sizeof(buf));
		for (int i = 0; i < n; i++)
			d[i].ad_flags = R_OK;
		d[0].ad_name_offset = (unsigned int) (n * sizeof(struct accessx_descriptor));
		strcpy(buf + d[0].ad_name_offset, file);
		size = d[0].ad_name_offset + strlen(file) + 1;
		res[0] = res[99] = -1;
		errno = 0;
		ret = accessx_np(d, size, res, 0);
		if (n == 100)
			check(ret == 0 && res[0] == 0 && res[99] == 0, "ACCESSX_MAX_DESCRIPTORS descriptors are answered");
		else
			check_fails(ret, ENOMEM, "more than ACCESSX_MAX_DESCRIPTORS is ENOMEM");
	}
}

// what only the superuser gets, and what even it doesn't
static void as_root(const char* dir, const char* file)
{
	struct request r = { 0 };
	int res[8];

	add(&r, file, W_OK);
	add(&r, NULL, _READ_OK | _WRITE_OK | _DELETE_OK | _CHOWN_OK);
	int ret = run(&r, res);
	check_errno(ret == 0 ? res[0] : -1, 0, "root: a 0644 file is writable");
	check_errno(ret == 0 ? res[1] : -1, 0, "root: a file can be read, written, deleted and given away (same-path entry)");

	// the superuser can't execute a file that no one may: EPERM, not EACCES
	check_errno(ask(file, X_OK), EPERM, "root: executing a 0644 file is EPERM");

	// immutable and append-only files hold for the superuser too: vnode_authorize_checkimmutable
	char frozen_dir[PATH_MAX], in_frozen[PATH_MAX];
	snprintf(frozen_dir, sizeof(frozen_dir), "%s/frozen", dir);
	snprintf(in_frozen, sizeof(in_frozen), "%s/file", frozen_dir);
	mkdir(frozen_dir, 0755);
	write_file(in_frozen, 0644);
	if (set_linux_flags(in_frozen, LINUX_FS_IMMUTABLE_FL) < 0)
		skip("immutable and append-only files", "the file system has no chattr flags");
	else
	{
		check_errno(ask(in_frozen, W_OK), EPERM, "root: writing an immutable file is EPERM");
		check_errno(ask(in_frozen, R_OK), 0, "root: an immutable file is readable");
		check_errno(ask(in_frozen, _DELETE_OK), EPERM, "root: deleting an immutable file is EPERM");
		set_linux_flags(in_frozen, LINUX_FS_APPEND_FL);
		check_errno(ask(in_frozen, W_OK), EPERM, "root: writing over an append-only file is EPERM");
		check_errno(ask(in_frozen, _APPEND_OK), 0, "root: appending to an append-only file");
		set_linux_flags(in_frozen, 0);
		set_linux_flags(frozen_dir, LINUX_FS_IMMUTABLE_FL);
		check_errno(ask(in_frozen, _DELETE_OK), EPERM, "root: deleting from an immutable directory is EPERM");
		check_errno(ask(in_frozen, W_OK), 0, "root: an immutable directory's file is still writable");
		set_linux_flags(frozen_dir, LINUX_FS_APPEND_FL);
		check_errno(ask(frozen_dir, W_OK), 0, "root: adding to an append-only directory");
		check_errno(ask(in_frozen, _DELETE_OK), EPERM, "root: deleting from an append-only directory is EPERM");
		set_linux_flags(frozen_dir, 0);
	}
	unlink(in_frozen);
	rmdir(frozen_dir);
}

static void as_nobody(const char* dir, const char* file, const char* open_dir, const char* in_open,
	const char* in_sticky, const char* mine_in_sticky, const char* in_their_sticky)
{
	struct request r = { 0 };
	int res[8];

	// a root-owned 0644 file: readable, not writable; its 0755 root-owned directory doesn't let us delete it
	add(&r, file, R_OK);
	add(&r, NULL, W_OK);
	add(&r, NULL, _DELETE_OK);
	add(&r, NULL, _WATTR_OK);
	add(&r, NULL, _RATTR_OK | _RPERM_OK);
	add(&r, NULL, _CHOWN_OK);
	add(&r, NULL, X_OK);
	add(&r, dir, _WRITE_OK);
	int ret = run(&r, res);
	check(ret == 0, "accessx_np succeeds as a non-root uid");
	if (ret == 0)
	{
		check_errno(res[0], 0, "non-root: a 0644 file is readable");
		check_errno(res[1], EACCES, "non-root: a root-owned 0644 file isn't writable (EACCES)");
		check_errno(res[2], EACCES, "non-root: _DELETE_OK needs the parent to be writable");
		check_errno(res[3], EACCES, "non-root: someone else's file's attributes aren't writable");
		check_errno(res[4], 0, "non-root: anyone may read attributes and permissions");
		check_errno(res[5], EACCES, "non-root: _CHOWN_OK is the superuser's");
		check_errno(res[6], EACCES, "non-root: executing a 0644 file is EACCES (EPERM is the superuser's)");
		check_errno(res[7], EACCES, "non-root: a 0755 root-owned directory isn't writable");
	}
	else
		printf("# accessx_np: %s\n", strerror(errno));

	// _DELETE_OK is decided by the parent: a 0777 directory lets anyone delete, a sticky one only the file's
	// and the directory's owners. Our own file is made here: Darling's chown() changes nothing.
	write_file(mine_in_sticky, 0644);
	memset(&r, 0, sizeof(r));
	add(&r, in_open, _DELETE_OK);
	add(&r, in_sticky, _DELETE_OK);
	add(&r, mine_in_sticky, _DELETE_OK);
	add(&r, NULL, _WATTR_OK | _WPERM_OK);
	add(&r, open_dir, _WRITE_OK | _DELETE_OK);
	ret = run(&r, res);
	check_errno(ret == 0 ? res[0] : -1, 0, "non-root: deleting from a 0777 directory");
	check_errno(ret == 0 ? res[1] : -1, EACCES, "non-root: not deleting someone else's file from a sticky directory");
	check_errno(ret == 0 ? res[2] : -1, 0, "non-root: deleting our own file from a sticky directory");
	check_errno(ret == 0 ? res[3] : -1, 0, "non-root: the owner may change a file's attributes and permissions");
	check_errno(ret == 0 ? res[4] : -1, EACCES, "non-root: deleting a directory needs its parent's write permission");
	if (in_their_sticky)
		check_errno(ask(in_their_sticky, _DELETE_OK), 0,
			"non-root: a sticky directory's owner may delete someone else's file in it");
	else
		skip("a sticky directory's owner deletes someone else's file", "the directory couldn't be given to uid 65534");
}

// real uid 65534, effective uid 0: faccessat(AT_EACCESS) asks as the effective ids, accessx_np and faccessat as the
// real ones
static void real_nobody_effective_root(const char* file)
{
	check(faccessat(AT_FDCWD, file, W_OK, AT_EACCESS) == 0, "faccessat(AT_EACCESS): effective root may write a 0644 file");
	errno = 0;
	check_fails(faccessat(AT_FDCWD, file, W_OK, 0), EACCES, "faccessat: real uid 65534 may not");
	check_errno(ask(file, W_OK), EACCES, "accessx_np checks as the real uid 65534");
}

// clonefile's answers before VNOP_CLONEFILE
static void clones(const char* dir, const char* file)
{
	char dst[PATH_MAX], copy[PATH_MAX], other[PATH_MAX];
	int fds[2];

	snprintf(dst, sizeof(dst), "%s/clone", dir);
	snprintf(copy, sizeof(copy), "%s/copy", dir);

	errno = 0;
	check_fails(clonefile(file, dst, 0), ENOTSUP, "clonefile is ENOTSUP: no clones on this volume");
	errno = 0;
	check_fails(clonefile(file, file, 0), EEXIST, "clonefile onto an existing name is EEXIST");
	errno = 0;
	check_fails(clonefile(dst, copy, 0), ENOENT, "clonefile of a missing file is ENOENT");
	errno = 0;
	check_fails(clonefile(file, dst, 0x100), EINVAL, "clonefile with an unknown flag is EINVAL");

	// what may be cloned: a symbolic link itself (even a dangling one) and a directory, not the root
	char link[PATH_MAX], dangling[PATH_MAX];
	snprintf(link, sizeof(link), "%s/link", dir);
	snprintf(dangling, sizeof(dangling), "%s/dangling", dir);
	symlink("file", link);
	symlink("nowhere", dangling);
	check_fails(clonefile(link, dst, CLONE_NOFOLLOW), ENOTSUP, "clonefile of a symbolic link itself is ENOTSUP");
	check_fails(clonefile(dangling, dst, CLONE_NOFOLLOW), ENOTSUP, "clonefile of a dangling link itself is ENOTSUP");
	check_fails(clonefile(dangling, dst, 0), ENOENT, "clonefile through a dangling link is ENOENT");
	check_fails(clonefile(dir, dst, 0), ENOTSUP, "clonefile of a directory is ENOTSUP");
	check_fails(clonefile("/", dst, 0), EINVAL, "clonefile of the root is EINVAL");
	// another mount: the prefix's /private/tmp, when the test runs outside it. EEXIST comes first: Biome retries
	// another name on it.
	snprintf(other, sizeof(other), "/private/tmp/access_extended.%d", getpid());
	if (strncmp(dir, "/private/", 9) != 0)
	{
		check_fails(clonefile(file, other, 0), EXDEV, "clonefile to another mount is EXDEV");
		write_file(other, 0644);
		check_fails(clonefile(file, other, 0), EEXIST, "clonefile onto an existing name on another mount is EEXIST");
		unlink(other);
	}
	else
		skip("clonefile to another mount", "the test directory is on /private/tmp's mount");
	// the "namei: path too long" answer, for either path
	char long_path[1100];
	memset(long_path, 'a', sizeof(long_path) - 1);
	long_path[0] = '/';
	long_path[sizeof(long_path) - 1] = '\0';
	check_fails(clonefile(long_path, dst, 0), ENAMETOOLONG, "clonefile of a MAXPATHLEN-long source is ENAMETOOLONG");
	check_fails(clonefile(file, long_path, 0), ENAMETOOLONG, "clonefile to a MAXPATHLEN-long target is ENAMETOOLONG");
	// a relative path from a descriptor that isn't open
	check_fails(clonefileat(987, "file", AT_FDCWD, dst, 0), EBADF, "clonefileat from a closed descriptor is EBADF");

	int fd = open(file, O_RDONLY);
	errno = 0;
	check_fails(fclonefileat(fd, AT_FDCWD, dst, 0), ENOTSUP, "fclonefileat is ENOTSUP");
	close(fd);
	fd = open(file, O_WRONLY);
	errno = 0;
	check_fails(fclonefileat(fd, AT_FDCWD, dst, 0), EBADF, "fclonefileat of a write-only file is EBADF");
	close(fd);
	fd = open(dir, O_RDONLY);
	check_fails(fclonefileat(fd, AT_FDCWD, dst, 0), ENOTSUP, "fclonefileat of a directory is ENOTSUP");
	close(fd);
	fd = open(link, O_RDONLY | O_SYMLINK);
	check_fails(fd < 0 ? 0 : fclonefileat(fd, AT_FDCWD, dst, 0), ENOTSUP, "fclonefileat of a symbolic link (O_SYMLINK) is ENOTSUP");
	close(fd);
	pipe(fds);
	errno = 0;
	check_fails(fclonefileat(fds[0], AT_FDCWD, dst, 0), ENOTSUP, "fclonefileat of a pipe is ENOTSUP");
	close(fds[0]);
	close(fds[1]);

	// faccessat(AT_SYMLINK_NOFOLLOW) asks about the link itself, which exists
	check(faccessat(AT_FDCWD, dangling, F_OK, AT_SYMLINK_NOFOLLOW) == 0, "faccessat(AT_SYMLINK_NOFOLLOW) of a dangling link");
	unlink(link);
	unlink(dangling);

	// the callers' fallback: copyfile's COPYFILE_CLONE copies when there is no clone
	char buf[16] = { 0 };
	check(copyfile(file, copy, NULL, COPYFILE_CLONE) == 0, "copyfile(COPYFILE_CLONE) falls back to a copy");
	fd = open(copy, O_RDONLY);
	check(fd >= 0 && read(fd, buf, sizeof(buf)) == 5 && memcmp(buf, "hello", 5) == 0, "the copy has the data");
	close(fd);
	unlink(copy);
}

// CLONE_NOFOLLOW_ANY (no symbolic link anywhere) and CLONE_RESOLVE_BENEATH (nothing outside the directory)
static void restricted_lookups(const char* dir)
{
	char sub[PATH_MAX], subfile[PATH_MAX], sublink[PATH_MAX], through[PATH_MAX], dst[PATH_MAX], new_through[PATH_MAX];
	char link_to_file[PATH_MAX], abs_link[PATH_MAX], up_link[PATH_MAX];

	snprintf(sub, sizeof(sub), "%s/sub", dir);
	snprintf(subfile, sizeof(subfile), "%s/sub/f", dir);
	snprintf(sublink, sizeof(sublink), "%s/sublink", dir);
	snprintf(through, sizeof(through), "%s/sublink/f", dir);
	snprintf(new_through, sizeof(new_through), "%s/sublink/new", dir);
	snprintf(link_to_file, sizeof(link_to_file), "%s/flink", dir);
	snprintf(abs_link, sizeof(abs_link), "%s/abslink", dir);
	snprintf(up_link, sizeof(up_link), "%s/uplink", dir);
	snprintf(dst, sizeof(dst), "%s/clone", dir);
	mkdir(sub, 0755);
	write_file(subfile, 0644);
	symlink("sub", sublink);
	symlink("sub/f", link_to_file);
	symlink(sub, abs_link);
	symlink("..", up_link);

	check_fails(clonefile(through, dst, 0), ENOTSUP, "clonefile through a directory's symbolic link is ENOTSUP");
	check_fails(clonefile(through, dst, CLONE_NOFOLLOW_ANY), ELOOP, "CLONE_NOFOLLOW_ANY: a link on the source's path is ELOOP");
	check_fails(clonefile(link_to_file, dst, CLONE_NOFOLLOW_ANY), ELOOP, "CLONE_NOFOLLOW_ANY: following the source link is ELOOP");
	check_fails(clonefile(link_to_file, dst, CLONE_NOFOLLOW_ANY | CLONE_NOFOLLOW), ENOTSUP,
		"CLONE_NOFOLLOW_ANY with CLONE_NOFOLLOW: the link itself is ENOTSUP");
	check_fails(clonefile(subfile, new_through, CLONE_NOFOLLOW_ANY), ELOOP, "CLONE_NOFOLLOW_ANY: a link on the target's path is ELOOP");
	char sublink_slash[PATH_MAX];
	snprintf(sublink_slash, sizeof(sublink_slash), "%s/", sublink);
	check_fails(clonefile(sublink_slash, dst, CLONE_NOFOLLOW_ANY | CLONE_NOFOLLOW), ELOOP,
		"CLONE_NOFOLLOW_ANY: a link before a trailing slash is followed: ELOOP");

	int dfd = open(dir, O_RDONLY);
	check_fails(clonefileat(dfd, "sub/f", dfd, "clone", CLONE_RESOLVE_BENEATH), ENOTSUP, "CLONE_RESOLVE_BENEATH: inside is ENOTSUP");
	check_fails(clonefileat(dfd, "sub/../sub/f", dfd, "clone", CLONE_RESOLVE_BENEATH), ENOTSUP,
		"CLONE_RESOLVE_BENEATH: a .. that stays inside is ENOTSUP");
	check_fails(clonefileat(dfd, "sublink/f", dfd, "clone", CLONE_RESOLVE_BENEATH), ENOTSUP,
		"CLONE_RESOLVE_BENEATH: a relative link inside is ENOTSUP");
	check_fails(clonefileat(dfd, subfile, dfd, "clone", CLONE_RESOLVE_BENEATH), ENOTCAPABLE,
		"CLONE_RESOLVE_BENEATH: an absolute source is ENOTCAPABLE");
	check_fails(clonefileat(dfd, "../x", dfd, "clone", CLONE_RESOLVE_BENEATH), ENOTCAPABLE,
		"CLONE_RESOLVE_BENEATH: a .. above the directory is ENOTCAPABLE");
	check_fails(clonefileat(dfd, "abslink/f", dfd, "clone", CLONE_RESOLVE_BENEATH), ENOTCAPABLE,
		"CLONE_RESOLVE_BENEATH: a link to an absolute path is ENOTCAPABLE");
	check_fails(clonefileat(dfd, "uplink/x", dfd, "clone", CLONE_RESOLVE_BENEATH), ENOTCAPABLE,
		"CLONE_RESOLVE_BENEATH: a link up and out is ENOTCAPABLE");
	check_fails(clonefileat(dfd, "sub/f", dfd, "../clone", CLONE_RESOLVE_BENEATH), ENOTCAPABLE,
		"CLONE_RESOLVE_BENEATH: a target outside is ENOTCAPABLE");
	check_fails(clonefileat(dfd, "sub/../../x", dfd, "clone", CLONE_RESOLVE_BENEATH), ENOTCAPABLE,
		"CLONE_RESOLVE_BENEATH: a .. back out after going in is ENOTCAPABLE");
	// the directory is checked before the path's components
	check_fails(clonefileat(987, "../x", dfd, "clone", CLONE_RESOLVE_BENEATH), EBADF,
		"CLONE_RESOLVE_BENEATH from a closed descriptor is EBADF");
	int ffd = open(subfile, O_RDONLY);
	check_fails(clonefileat(ffd, "../x", dfd, "clone", CLONE_RESOLVE_BENEATH), ENOTDIR,
		"CLONE_RESOLVE_BENEATH from a file's descriptor is ENOTDIR");
	close(ffd);
	// a link's text and the rest of the path within MAXPATHLEN
	char long_link[PATH_MAX], long_target[1001];
	memset(long_target, 'b', sizeof(long_target) - 1);
	long_target[sizeof(long_target) - 1] = '\0';
	snprintf(long_link, sizeof(long_link), "%s/longlink", dir);
	symlink(long_target, long_link);
	check_fails(clonefileat(dfd, "longlink/cccccccccccccccccccccccccccccccccccccccc", dfd, "clone", CLONE_RESOLVE_BENEATH),
		ENAMETOOLONG, "CLONE_RESOLVE_BENEATH: a link's text and the rest over MAXPATHLEN is ENAMETOOLONG");
	unlink(long_link);
	close(dfd);

	unlink(sublink);
	unlink(link_to_file);
	unlink(abs_link);
	unlink(up_link);
	unlink(subfile);
	rmdir(sub);
}

int main(void)
{
	// somewhere a non-root uid can reach: the prefix is usually in root's 0700 home directory, and
	// /Volumes/SystemRoot is the Linux root
	struct stat bst;
	const char* base = stat("/Volumes/SystemRoot/tmp", &bst) == 0 ? "/Volumes/SystemRoot/tmp" : "/private/tmp";
	char dir[PATH_MAX], file[PATH_MAX], missing[PATH_MAX], open_dir[PATH_MAX], sticky_dir[PATH_MAX];
	char in_open[PATH_MAX], in_sticky[PATH_MAX], mine_in_sticky[PATH_MAX], their_sticky[PATH_MAX];
	char in_their_sticky[PATH_MAX];
	const int linux_root = __linux_syscall(LINUX_NR_getuid) == 0;

	snprintf(dir, sizeof(dir), "%s/access_extended.XXXXXX", base);
	if (!mkdtemp(dir))
	{
		perror("mkdtemp");
		return 1;
	}
	chmod(dir, 0755);
	snprintf(file, sizeof(file), "%s/file", dir);
	snprintf(missing, sizeof(missing), "%s/missing", dir);
	snprintf(open_dir, sizeof(open_dir), "%s/open", dir);
	snprintf(sticky_dir, sizeof(sticky_dir), "%s/sticky", dir);
	snprintf(their_sticky, sizeof(their_sticky), "%s/their_sticky", dir);
	snprintf(in_open, sizeof(in_open), "%s/file", open_dir);
	snprintf(in_sticky, sizeof(in_sticky), "%s/file", sticky_dir);
	snprintf(mine_in_sticky, sizeof(mine_in_sticky), "%s/mine", sticky_dir);
	snprintf(in_their_sticky, sizeof(in_their_sticky), "%s/file", their_sticky);
	write_file(file, 0644);
	mkdir(open_dir, 0777);
	chmod(open_dir, 0777);
	mkdir(sticky_dir, 0777);
	chmod(sticky_dir, 01777);
	mkdir(their_sticky, 0777);
	chmod(their_sticky, 01777);
	write_file(in_open, 0644);
	write_file(in_sticky, 0644);
	write_file(in_their_sticky, 0644);

	anyone(dir, file, missing);
	malformed(file, missing);
	clones(dir, file);
	restricted_lookups(dir);

	if (!linux_root)
	{
		skip("the superuser's checks", "not Linux root");
		skip("someone else's files (a child as Linux uid 65534)", "not Linux root");
	}
	else
	{
		as_root(dir, file);

		// someone else's files: the same questions as a non-root Linux uid, in a child
		int given = set_linux_owner(their_sticky, NOBODY) == 0;
		for (int which = 0; which < 2; which++)
		{
			int pipefd[2];
			pipe(pipefd);
			pid_t pid = fork();
			if (pid == 0)
			{
				close(pipefd[0]);
				failures = 0; // the parent's count is the parent's
				// 0: every id 65534; 1: the real uid 65534 and the effective one still 0
				int euid = which == 0 ? NOBODY : 0;
				if (__linux_syscall(LINUX_NR_setgroups, 0, 0) < 0 || __linux_syscall(LINUX_NR_setresgid, NOBODY, NOBODY, NOBODY) < 0 ||
					__linux_syscall(LINUX_NR_setresuid, NOBODY, euid, euid) < 0)
					check(0, "switching to Linux uid 65534");
				else if (which == 0)
					as_nobody(dir, file, open_dir, in_open, in_sticky, mine_in_sticky, given ? in_their_sticky : NULL);
				else
					real_nobody_effective_root(file);
				write(pipefd[1], &failures, sizeof(failures));
				_exit(0);
			}
			close(pipefd[1]);
			int child_failures = 0;
			if (read(pipefd[0], &child_failures, sizeof(child_failures)) != sizeof(child_failures))
				check(0, "the non-root child reported back");
			failures += child_failures;
			close(pipefd[0]);
			waitpid(pid, NULL, 0);
		}
	}

	unlink(in_open);
	unlink(in_sticky);
	unlink(mine_in_sticky);
	unlink(in_their_sticky);
	rmdir(open_dir);
	rmdir(sticky_dir);
	rmdir(their_sticky);
	unlink(file);
	rmdir(dir);
	return failures;
}
