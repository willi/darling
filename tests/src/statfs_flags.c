// statfs()'s f_flags are XNU's MNT_* flags, translated from Linux's own: checked against Linux's view of each mount
// (/proc/self/mountinfo). Linux's ST_* bits used to be copied as they were, and ST_RELATIME (0x1000) is MNT_LOCAL: a
// relatime mount read as local by chance, and a noatime one (ext4 mounted noatime) as a network volume, where Core
// Data puts a SQLite store behind proxy locking and a device's first boot hung in data migration.
//
// Every mount is checked: MNT_LOCAL exactly when it isn't a network file system, MNT_NOATIME exactly when it's mounted
// noatime, and MNT_RDONLY, MNT_NOSUID, MNT_NODEV and MNT_NOEXEC as its options say. getattrlist()'s
// ATTR_VOL_MOUNTFLAGS and getfsstat() report the same flags. Noatime and network mounts to check are up to the caller
// (mount -t tmpfs -o noatime tmpfs /mnt): without one, that check is skipped. statfs() blocks on a hard-mounted NFS
// server that doesn't answer, as it does on Linux.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <unistd.h>

// Linux's root, as a Darling process sees it
#define SYSTEM_ROOT "/Volumes/SystemRoot"

static int failures;

static void check(int ok, const char* what, const char* on)
{
	printf("%s - %s%s%s\n", ok ? "ok" : "not ok", what, on ? ": " : "", on ? on : "");
	if (!ok)
		failures++;
}

// whether a comma-separated option list has this option
static int has_option(const char* options, const char* name)
{
	size_t n = strlen(name);
	for (const char* p = options; p; p = strchr(p, ','), p = p ? p + 1 : NULL) {
		if (strncmp(p, name, n) == 0 && (p[n] == ',' || p[n] == '\0'))
			return 1;
	}
	return 0;
}

// network file systems, by the type mountinfo and f_fstypename name them (libsystem_kernel goes by their magic
// numbers). The FUSE ones mirror libsystem_kernel's list: FUSE file systems share one magic number.
static int is_network(const char* type)
{
	static const char* const network[] = { "nfs", "nfs4", "cifs", "smb3", "smbfs", "ceph", "afs", "coda", "ncpfs",
		"lustre", "gpfs", "beegfs", "fuse.sshfs", "fuse.s3fs", "fuse.gcsfuse", "fuse.rclone", "fuse.glusterfs",
		"fuse.ceph-fuse", "fuse.smbnetfs", "fuse.curlftpfs", "fuse.juicefs", "fuse.goofys", "fuse.s3backer" };
	for (unsigned i = 0; i < sizeof(network) / sizeof(network[0]); i++) {
		if (strcmp(type, network[i]) == 0)
			return 1;
	}
	return 0;
}

struct mount {
	char on[MAXPATHLEN], options[512], type[64], super[512];
};

// whether a later mount hides this one: one on its mount point or on a directory above it
static int hidden(const char* on, const char* later)
{
	size_t n = strlen(later);
	return strcmp(later, "/") == 0 || (strncmp(on, later, n) == 0 && (on[n] == '/' || on[n] == '\0'));
}

// the mounts in Linux's mountinfo whose mount point statfs() reaches: a mount point mounted on more than once, or
// under a later mount, is left out (which of them statfs() sees isn't the order alone), as are mount points with
// escaped characters
static int read_mounts(struct mount** out)
{
	FILE* f = fopen(SYSTEM_ROOT "/proc/self/mountinfo", "r");
	char* line = NULL;
	size_t size = 0;
	int n = 0, cap = 0;
	struct mount* m = NULL;

	if (!f)
		return -1;
	while (getline(&line, &size, f) > 0) {
		struct mount cur;
		char* sep = strstr(line, " - ");
		if (!sep || sscanf(line, "%*s %*s %*s %*s %1023s %511s", cur.on, cur.options) != 2 ||
			sscanf(sep + 3, "%63s %*s %511s", cur.type, cur.super) != 2 || strchr(cur.on, '\\'))
			continue;
		if (n == cap) {
			struct mount* more = realloc(m, (cap = cap ? 2 * cap : 32) * sizeof(*m));
			if (!more) {
				n = -1;
				break;
			}
			m = more;
		}
		m[n++] = cur;
	}
	free(line);
	fclose(f);

	int kept = 0;
	for (int i = 0; i < n; i++) {
		int skip = 0;
		for (int j = 0; j < n && !skip; j++)
			skip = j != i && (strcmp(m[i].on, m[j].on) == 0 || (j > i && hidden(m[i].on, m[j].on)));
		if (!skip)
			m[kept++] = m[i];
	}
	*out = m;
	return n < 0 ? -1 : kept;
}

// getattrlist()'s ATTR_VOL_MOUNTFLAGS: statfs()'s f_flags, MNT_LOCAL included
static void check_getattrlist(const char* path, const struct statfs* st)
{
	struct attrlist attrs = { .bitmapcount = ATTR_BIT_MAP_COUNT, .volattr = ATTR_VOL_INFO | ATTR_VOL_MOUNTFLAGS };
	struct {
		uint32_t length;
		uint32_t flags;
	} __attribute__((packed)) vol;

	memset(&vol, 0, sizeof(vol));
	check(getattrlist(path, &attrs, &vol, sizeof(vol), 0) == 0 && vol.length == sizeof(vol) &&
			  (vol.flags & MNT_VISFLAGMASK) == st->f_flags && (vol.flags & MNT_LOCAL),
		"getattrlist()'s ATTR_VOL_MOUNTFLAGS is statfs()'s f_flags", path);
}

static void check_flags(const struct mount* m, uint32_t flags)
{
	char what[MAXPATHLEN + 128];

	snprintf(what, sizeof(what), "%s (%s, %s)", m->on, m->type, m->options);
	check(((flags & MNT_LOCAL) != 0) == !is_network(m->type), "MNT_LOCAL exactly when not a network file system",
		what);
	check(!(flags & MNT_NOATIME) == !has_option(m->options, "noatime"), "MNT_NOATIME exactly when noatime", what);
	check(!(flags & MNT_RDONLY) == !(has_option(m->options, "ro") || has_option(m->super, "ro")),
		"MNT_RDONLY exactly when ro", what);
	check(!(flags & MNT_NOSUID) == !has_option(m->options, "nosuid"), "MNT_NOSUID exactly when nosuid", what);
	check(!(flags & MNT_NODEV) == !has_option(m->options, "nodev"), "MNT_NODEV exactly when nodev", what);
	check(!(flags & MNT_NOEXEC) == !has_option(m->options, "noexec"), "MNT_NOEXEC exactly when noexec", what);
}

int main(void)
{
	struct mount* mounts;
	int n, checked = 0, relatime = 0, noatime = 0, network = 0;
	char noatime_path[MAXPATHLEN + sizeof(SYSTEM_ROOT)] = "";

	// each line out before a crash can lose it
	setvbuf(stdout, NULL, _IOLBF, 0);
	n = read_mounts(&mounts);
	check(n > 0, "Linux's mounts read from " SYSTEM_ROOT "/proc/self/mountinfo", NULL);
	for (int i = 0; i < n; i++) {
		char path[MAXPATHLEN + sizeof(SYSTEM_ROOT)];
		struct statfs st;

		snprintf(path, sizeof(path), SYSTEM_ROOT "%s", mounts[i].on);
		// a mount this process can't reach (under another one, or permissions) has nothing to compare, nor has one
		// another is over (mount --move breaks mountinfo's order). "/" is the root's names, which statfs() gives a
		// mount it can't name: before Linux 5.8, every one
		size_t len = strlen(path);
		if (len > 1 && path[len - 1] == '/')
			path[len - 1] = '\0';
		if (statfs(path, &st) != 0 || (strcmp(st.f_mntonname, path) != 0 && strcmp(st.f_mntonname, "/") != 0))
			continue;
		check_flags(&mounts[i], st.f_flags);
		checked++;
		relatime += has_option(mounts[i].options, "relatime");
		network += is_network(mounts[i].type);
		if (has_option(mounts[i].options, "noatime") && !is_network(mounts[i].type) && noatime++ == 0)
			strcpy(noatime_path, path);
	}
	check(checked > 0, "a mount was checked", NULL);
	printf("# note %d mounts checked: %d relatime, %d noatime, %d network\n", checked, relatime, noatime, network);
	if (noatime > 0)
		check(1, "a noatime mount was checked", NULL);
	else
		printf("ok - # SKIP no noatime mount to check (mount -t tmpfs -o noatime tmpfs /mnt)\n");
	if (network > 0)
		check(1, "a network mount was checked", NULL);
	else
		printf("ok - # SKIP no network mount to check\n");

	// the other ways a process reads the same flags
	struct statfs st;
	check(statfs("/", &st) == 0, "statfs(\"/\")", NULL);
	check_getattrlist("/", &st);
	if (noatime_path[0] && statfs(noatime_path, &st) == 0)
		check_getattrlist(noatime_path, &st);

	int count = getfsstat(NULL, 0, MNT_NOWAIT);
	struct statfs* all = count > 0 ? calloc(count, sizeof(*all)) : NULL;
	count = all ? getfsstat(all, count * (int) sizeof(*all), MNT_NOWAIT) : -1;
	check(count > 0, "getfsstat()", NULL);
	for (int i = 0; i < count; i++) {
		check(((all[i].f_flags & MNT_LOCAL) != 0) == !is_network(all[i].f_fstypename),
			"getfsstat()'s MNT_LOCAL exactly when not a network file system", all[i].f_mntonname);
	}

	return failures;
}
