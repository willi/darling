// mldr keeps the iOS Simulator's dyld shared cache range free (src/startup/mldr/shared_region.c). dyld_sim maps the
// cache with MAP_FIXED at the addresses its header gives, from 0x7FF800000000 on, and Linux puts each exec's stack and
// vDSO at a random place in the 16 GiB under the top of the address space: about 1 exec in 15, with the cache of an
// iOS 26 runtime, dyld_sim would map over them. For a process that will map a cache, mldr now takes another layout
// when anything is in one of the cache's ranges, and reserves them (PROT_NONE) when nothing is.
//
// It writes caches that are only headers, and spawns itself with DYLD_ROOT_PATH (dyld_sim's sign of a simulator
// process; the root here has nothing in it, so this program's own libraries load from the usual paths) and the cache:
// - a cache laid out as an iOS 26 runtime's, a main file and five sub-caches, at the root's com.apple.dyld path: in
//   every child, each range is reserved, with no commit charge, nothing else in it, and dyld_sim's MAP_FIXED over it
//   works.
// - "wide", one 2 GiB range in the stack's 16 GiB, from DYLD_SHARED_CACHE_DIR: about an eighth of the layouts take it.
//   With DYLD_SHARED_REGION=avoid (dyld_sim maps no cache, mldr reserves nothing) some children have something in the
//   range: the check can fail. Without it, none: mldr took another layout each time.
// (mldr giving up after 8 layouts can't be forced from here: a range the stack is always in reaches mldr's own stack,
// which mldr refuses to reserve for.)
// The children's address spaces are read in /proc, through /Volumes/SystemRoot (the Linux root).
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
extern int _NSGetExecutablePath(char* buf, uint32_t* size); // <mach-o/dyld.h>

#define G (1ULL << 30)
#define BASE 0x7ff800000000ULL
#define STACK_LO 0x7ffc00000000ULL // the top 16 GiB, where Linux puts the stack
#define MARGIN 0x100000ULL // mldr keeps the stack's guard gap clear too
#define MAX_RANGES 32

// the children's exit statuses
enum { CHILD_OK, CHILD_NOT_RESERVED = 10, CHILD_OCCUPIED, CHILD_CHARGED, CHILD_MAP_FIXED, CHILD_NO_MAPS, CHILD_USAGE };

static int failed;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failed++;
}

struct range {
	unsigned long long lo, hi;
};

// ---- the caches

static void put32(unsigned char* p, uint32_t v) { memcpy(p, &v, sizeof(v)); }
static void put64(unsigned char* p, uint64_t v) { memcpy(p, &v, sizeof(v)); }

// One cache file: a dyld_cache_header (dyld-1378 dyld_cache_format.h; the fields mldr reads) and its mapping table,
// then, for a main file with sub-caches, their entries. dyn: the dynamic data page's offset from BASE, 0 for none.
static int write_cache_file(const char* path, const struct range* m, int n, int subs, uint64_t dyn)
{
	unsigned char buf[0x1000];
	const uint32_t moff = 0x228, soff = moff + 32 * n;

	memset(buf, 0, sizeof(buf));
	memcpy(buf, "dyld_v1  x86_64", 16);
	put32(buf + 0x10, moff);
	put32(buf + 0x14, n);
	put32(buf + 0xdc, 0x1700); // dylibsExpectedOnDisk, simulator, locallyBuiltCache: an update_dyld_sim_shared_cache's
	put64(buf + 0xe0, BASE);
	put64(buf + 0xe8, 0x600000000);
	put32(buf + 0x188, subs ? soff : 0);
	put32(buf + 0x18c, subs);
	put64(buf + 0x1f0, dyn);
	put64(buf + 0x1f8, dyn ? 0x4000 : 0);
	for (int i = 0; i < n; ++i) {
		put64(buf + moff + 32 * i, m[i].lo);
		put64(buf + moff + 32 * i + 8, m[i].hi - m[i].lo);
	}
	for (int i = 0; i < subs; ++i) {
		put64(buf + soff + 56 * i + 16, (uint64_t)(i + 1) * 4 * G);
		snprintf((char*)buf + soff + 56 * i + 24, 32, ".%02d", i + 1);
	}

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	int ok = write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf);
	return close(fd) == 0 && ok ? 0 : -1;
}

// An iOS 26 runtime's layout (23F77, built here): a main file and five sub-caches, one per 4 GiB, each with text,
// two data mappings next to each other and linkedit; the dynamic data page after the last. Returns its ranges.
static int write_layout_cache(const char* path, struct range* all)
{
	char sub[1024];
	int n = 0;

	for (int f = 0; f <= 5; ++f) {
		unsigned long long b = BASE + (unsigned long long)f * 4 * G;
		struct range m[4] = {
			{ b, b + 0x20000000 },
			{ b + G, b + G + 0x3000000 },
			{ b + G + 0x3000000, b + G + 0x4000000 },
			{ b + 2 * G, b + 2 * G + 0xb000000 },
		};
		memcpy(all + n, m, sizeof(m));
		n += 4;
		if (f == 0)
			continue;
		snprintf(sub, sizeof(sub), "%s.%02d", path, f);
		if (write_cache_file(sub, m, 4, 0, 0) != 0)
			return -1;
	}
	uint64_t dyn = all[n - 1].hi - BASE;
	all[n++] = (struct range){ BASE + dyn, BASE + dyn + 0x4000 };
	return write_cache_file(path, all, 4, 5, dyn) == 0 ? n : -1;
}

// ---- the child: is its address space as mldr should have left it?

// Whether the VMAs in maps cover [lo, hi) with nothing but PROT_NONE private mappings (dyld_sim's MAP_FIXED replaces
// them; anything else there it would replace too).
static int reserved(char* maps, unsigned long long lo, unsigned long long hi)
{
	unsigned long long at = lo;
	for (char* line = maps; *line; line += strlen(line) + 1) {
		unsigned long long a, b;
		char perms[5];
		if (sscanf(line, "%llx-%llx %4s", &a, &b, perms) != 3 || b <= at || a >= hi)
			continue;
		if (a > at || strcmp(perms, "---p") != 0)
			return 0;
		at = b;
		if (at >= hi)
			return 1;
	}
	return 0;
}

// Whether a VMA that isn't a reservation is in a range, or the stack within MARGIN of one.
static int occupied(char* maps, const struct range* r, int n, int reserve)
{
	for (char* line = maps; *line; line += strlen(line) + 1) {
		unsigned long long a, b;
		char perms[5];
		if (sscanf(line, "%llx-%llx %4s", &a, &b, perms) != 3)
			continue;
		int stack = strstr(line, "[stack]") != NULL;
		if (reserve && strcmp(perms, "---p") == 0 && !strchr(line, '/') && !strchr(line, '['))
			continue;
		unsigned long long m = stack ? MARGIN : 0;
		for (int i = 0; i < n; ++i) {
			if (a < r[i].hi + m && b + m > r[i].lo)
				return 1;
		}
	}
	return 0;
}

// The file at path, one NUL-terminated string per line, then an empty one.
static char* slurp(const char* path)
{
	static char buf[1 << 20];
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;
	size_t len = 0;
	ssize_t k;
	while (len < sizeof(buf) - 2 && (k = read(fd, buf + len, sizeof(buf) - 2 - len)) > 0)
		len += k;
	close(fd);
	buf[len] = buf[len + 1] = '\0';
	for (char* p = buf; (p = strchr(p, '\n')); )
		*p++ = '\0';
	return buf;
}

// Whether a VMA starting in a range is charged against the commit limit ("ac" in its VmFlags, VM_ACCOUNT): under
// vm.overcommit_memory=2 a reservation that is would take the cache's size from what everything else may allocate.
static int charged(char* smaps, const struct range* r, int n)
{
	int in = 0;
	for (char* line = smaps; *line; line += strlen(line) + 1) {
		unsigned long long a, b;
		if (sscanf(line, "%llx-%llx ", &a, &b) == 2) {
			in = 0;
			for (int i = 0; i < n; ++i)
				in |= a >= r[i].lo && a < r[i].hi;
		} else if (in && strncmp(line, "VmFlags:", 8) == 0) {
			for (char* p = strstr(line, " ac"); p; p = strstr(p + 1, " ac")) {
				if (p[3] == ' ' || p[3] == '\0')
					return 1;
			}
		}
	}
	return 0;
}

// child reserve|free LO HI ...: "reserve": mldr should have reserved the ranges; "free": it shouldn't have (no cache),
// and the child only says whether anything is in them.
static int child(int argc, char** argv)
{
	struct range r[MAX_RANGES];
	int n = 0, reserve = strcmp(argv[2], "reserve") == 0;

	for (int i = 3; i + 1 < argc && n < MAX_RANGES; i += 2)
		r[n++] = (struct range){ strtoull(argv[i], NULL, 16), strtoull(argv[i + 1], NULL, 16) };
	if (n == 0)
		return CHILD_USAGE;

	char* maps = slurp("/Volumes/SystemRoot/proc/self/maps");
	if (!maps)
		return CHILD_NO_MAPS;
	if (occupied(maps, r, n, reserve))
		return CHILD_OCCUPIED;
	if (!reserve)
		return CHILD_OK;
	for (int i = 0; i < n; ++i) {
		if (!reserved(maps, r[i].lo, r[i].hi))
			return CHILD_NOT_RESERVED;
	}
	char* smaps = slurp("/Volumes/SystemRoot/proc/self/smaps");
	if (!smaps)
		return CHILD_NO_MAPS;
	if (charged(smaps, r, n))
		return CHILD_CHARGED;

	// what dyld_sim does: map over the reservation, with MAP_FIXED
	void* want = (void*)(uintptr_t)r[0].lo;
	size_t len = r[0].hi - r[0].lo;
	void* p = mmap(want, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	if (p != want)
		return CHILD_MAP_FIXED;
	((volatile char*)p)[len - 1] = 1;
	return CHILD_OK;
}

// ---- the parent

static char self[1024];

// Spawns this program as a child for the ranges, with DYLD_ROOT_PATH=root, the cache directory (NULL: none) and
// DYLD_SHARED_REGION=avoid if avoid. Returns its exit status, -1 if it didn't exit (or wasn't spawned).
static int spawn_child(const char* root, const char* dir, int avoid, const struct range* r, int n)
{
	char rootv[1100], dirv[1100], hex[MAX_RANGES * 2][20];
	char* argv[3 + MAX_RANGES * 2 + 1];
	char* envp[512];
	int a = 0, e = 0;

	argv[a++] = self;
	argv[a++] = "child";
	argv[a++] = avoid ? "free" : "reserve";
	for (int i = 0; i < n; ++i) {
		snprintf(hex[2 * i], sizeof(hex[0]), "%llx", r[i].lo);
		snprintf(hex[2 * i + 1], sizeof(hex[0]), "%llx", r[i].hi);
		argv[a++] = hex[2 * i];
		argv[a++] = hex[2 * i + 1];
	}
	argv[a] = NULL;

	for (char** v = environ; *v && e < 500; ++v) {
		if (strncmp(*v, "DYLD_ROOT_PATH=", 15) != 0 && strncmp(*v, "DYLD_SHARED_CACHE_DIR=", 22) != 0 &&
			strncmp(*v, "DYLD_SHARED_REGION=", 19) != 0)
			envp[e++] = *v;
	}
	snprintf(rootv, sizeof(rootv), "DYLD_ROOT_PATH=%s", root);
	envp[e++] = rootv;
	if (dir) {
		snprintf(dirv, sizeof(dirv), "DYLD_SHARED_CACHE_DIR=%s", dir);
		envp[e++] = dirv;
	}
	if (avoid)
		envp[e++] = "DYLD_SHARED_REGION=avoid";
	envp[e] = NULL;

	pid_t pid;
	int status;
	if (posix_spawn(&pid, self, NULL, NULL, argv, envp) != 0)
		return -1;
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

// Runs `runs` children; counts their exit statuses into ok (0), other (anything else), with the first other in first.
static void spawn_children(int runs, const char* root, const char* dir, int avoid, const struct range* r, int n,
	int* ok, int* occupied_n, int* other, int* first)
{
	*ok = *occupied_n = *other = 0;
	*first = 0;
	for (int i = 0; i < runs; ++i) {
		int s = spawn_child(root, dir, avoid, r, n);
		if (s == CHILD_OK)
			++*ok;
		else if (s == CHILD_OCCUPIED)
			++*occupied_n;
		else if (!(*other)++)
			*first = s;
	}
}

static int mkdirs(const char* base, const char* rel)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", base, rel);
	for (char* p = path + strlen(base) + 1; ; ++p) {
		if (*p == '/' || *p == '\0') {
			char c = *p;
			*p = '\0';
			if (mkdir(path, 0755) != 0 && errno != EEXIST)
				return -1;
			if (!(*p = c))
				return 0;
		}
	}
}

int main(int argc, char** argv)
{
	if (argc >= 3 && strcmp(argv[1], "child") == 0)
		return child(argc, argv);

	uint32_t size = sizeof(self);
	char dir[] = "/Volumes/SystemRoot/tmp/sim_shared_region.XXXXXX";
	char root[1024], noroot[1024], cache[1024], wide[1024], line[256];
	struct range layout[MAX_RANGES];
	int ok, occ, other, first, n;

	if (_NSGetExecutablePath(self, &size) != 0 || mkdtemp(dir) == NULL) {
		check(0, "setup: executable path, mkdtemp");
		return failed;
	}
	snprintf(root, sizeof(root), "%s/root", dir);
	snprintf(noroot, sizeof(noroot), "%s/noroot", dir);
	snprintf(cache, sizeof(cache), "%s/System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_x86_64", root);
	snprintf(wide, sizeof(wide), "%s/wide", dir);
	if (mkdirs(dir, "root/System/Library/Caches/com.apple.dyld") != 0 || mkdirs(dir, "wide") != 0 ||
		(n = write_layout_cache(cache, layout)) < 0) {
		check(0, "setup: the caches");
		goto out;
	}

	spawn_children(20, root, NULL, 0, layout, n, &ok, &occ, &other, &first);
	snprintf(line, sizeof(line), "an iOS 26 cache's %d ranges are reserved, alone, uncharged, and MAP_FIXED maps over "
		"them, in each of 20 children (%d did; %d had something in them; %d failed otherwise, the first with %d)", n, ok,
		occ, other, first);
	check(ok == 20, line);

	// one range: an eighth of the stack's 16 GiB
	struct range w = { STACK_LO + 4 * G, STACK_LO + 6 * G };
	char path[1100];
	snprintf(path, sizeof(path), "%s/dyld_sim_shared_cache_x86_64", wide);
	if (write_cache_file(path, &w, 1, 0, 0) != 0) {
		check(0, "setup: the wide cache");
		goto out;
	}
	spawn_children(200, noroot, wide, 1, &w, 1, &ok, &occ, &other, &first);
	snprintf(line, sizeof(line), "without a reservation, something is in a range in an eighth of the stack's 16 GiB "
		"in some of 200 children (%d; %d clear; %d failed, the first with %d)", occ, ok, other, first);
	check(occ > 0 && other == 0, line);
	spawn_children(200, noroot, wide, 0, &w, 1, &ok, &occ, &other, &first);
	snprintf(line, sizeof(line), "with it, the range is reserved and alone in each of 200 children (%d were; %d had "
		"something in it; %d failed otherwise, the first with %d)", ok, occ, other, first);
	check(ok == 200, line);

out:
	{
		char cmd[1200];
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
		system(cmd);
	}
	return failed;
}
