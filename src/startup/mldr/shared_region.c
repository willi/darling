/*
This file is part of Darling.

Darling is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Darling is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Darling.  If not, see <http://www.gnu.org/licenses/>.
*/

// The iOS Simulator's dyld (dyld_sim) maps its shared cache privately, with MAP_FIXED, at the addresses the cache's
// header gives: from 0x7FF800000000 on x86_64, one 4 GiB step per sub-cache, never slid. XNU keeps that range for the
// shared region. Linux doesn't: at exec it can put the process's stack and vDSO (in the 16 GiB under the top of the
// address space) or its first mappings there, and dyld_sim's MAP_FIXED would then silently replace them.
//
// So for a process that will map a simulator cache, mldr reads the cache's ranges, takes another address space
// layout when anything is in one of them (shared_region_reserve() says so; mldr.c's redraw_layout() execs again), and
// otherwise reserves them (PROT_NONE, no swap or commit charge) so that no later mapping lands there. dyld_sim's
// MAP_FIXED (each mapping's mmap, and the dynamic data page's in DynamicRegion::make()) replaces the reservation.
//
// Which cache: the one dyld_sim's CacheFinder takes (dyld-1378 dyld/DyldProcessConfig.cpp), for a process with
// DYLD_ROOT_PATH: $DYLD_ROOT_PATH/System/Library/Caches/com.apple.dyld/ if the cache file is there (whatever
// DYLD_SHARED_REGION says: dyld_sim ignores "avoid" for that one), else none with DYLD_SHARED_REGION=avoid, else
// $DYLD_SHARED_CACHE_DIR's. Their paths are the process's (vchrooted) paths; before mldr checks in with darlingserver
// it doesn't know the prefix, so it reads only those under /Volumes/SystemRoot, which is the Linux root. Nothing is
// reserved for a cache elsewhere: one in a prefix directory (tools/ios-sim's default SIM_DYLD_SHARED_CACHE_DIR), or
// the prefix's /System/Library/Caches/com.apple.dyld/, dyld_sim's last choice when DYLD_SHARED_CACHE_DIR is unset; nor
// when the runtime root itself is elsewhere, since mldr can't tell then which cache dyld_sim takes.
//
// It refuses (reserves nothing for) what dyld-1378 refuses (dyld/SharedCacheRuntime.cpp, preflightCacheFile and
// mapSplitCachePrivate): a file that isn't an x86_64 simulator cache, no mappings or more than 8 in a file, a header
// from before sub-cache suffixes, 128 or more sub-caches, a sub-cache missing or bad.

#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif
#include "shared_region.h"

// x86_64 only: mldr32 builds this file too, and a 32-bit process maps no simulator cache
#ifdef __x86_64__

#include <darling-config.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#	define MAP_FIXED_NOREPLACE 0x100000
#endif

#define CACHE_FILE "dyld_sim_shared_cache_x86_64"

// dyld_cache_header fields (dyld-1378 include/mach-o/dyld_cache_format.h), by offset. A field exists only in a header
// that reaches past it: mappingOffset is the header's size.
#define H_MAPPING_OFFSET        0x10
#define H_MAPPING_COUNT         0x14
#define H_FLAGS                 0xdc
#define H_SHARED_REGION_START   0xe0
#define H_SUBCACHE_ARRAY_OFFSET 0x188
#define H_SUBCACHE_ARRAY_COUNT  0x18c
#define H_CACHE_SUB_TYPE        0x1c8 // a header past it has dyld_subcache_entry's fileSuffix
#define H_DYNAMIC_DATA_OFFSET   0x1f0
#define H_SIZE                  0x200
#define FLAG_SIMULATOR          (1u << 9)
#define MAPPING_SIZE            32 // dyld_cache_mapping_info: address, size, fileOffset, maxProt, initProt
#define SUBCACHE_ENTRY_SIZE     56 // dyld_subcache_entry: uuid[16], cacheVMOffset, fileSuffix[32]
#define DYNAMIC_REGION_SIZE     0x4000 // DyldSharedCache::DynamicRegion::size(): all dyld_sim maps there

// dyld-1378's limits: DyldSharedCache::MaxMappings, and MAX_SUBCACHES (a count must be below it)
#define MAX_MAPPINGS  8
#define MAX_SUBCACHES 128
#define MAX_RANGES    (MAX_SUBCACHES * MAX_MAPPINGS + 1)
_Static_assert(MAX_RANGES == SHARED_REGION_MAX_RANGES, "shared_region.h's SHARED_REGION_MAX_RANGES");

// where dyld_sim maps a cache's first mapping (SHARED_REGION_BASE_X86_64); the others keep their offsets from it
#define SHARED_REGION_BASE 0x7ff800000000UL

// the top of a 64-bit process's address space (47 bits)
#define USER_TOP 0x800000000000UL

// the stack guard gap: no range comes within this of another mapping (the Linux stack grows down into it)
#define MARGIN 0x100000UL

struct range {
	unsigned long lo, hi;
};

static uint32_t get32(const unsigned char* p)
{
	uint32_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static uint64_t get64(const unsigned char* p)
{
	uint64_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

// The Linux path of a process path under SYSTEM_ROOT, with `leaf` appended (one "/" between them), in buf; NULL for
// any other path.
static const char* linux_path(char* buf, size_t size, const char* path, const char* leaf)
{
	const size_t k = sizeof(SYSTEM_ROOT) - 1;
	if (!path || strncmp(path, SYSTEM_ROOT, k) != 0 || path[k] != '/')
		return NULL;
	path += k; // keep the "/"
	size_t n = strlen(path);
	int r = snprintf(buf, size, "%s%s%s", path, path[n - 1] == '/' ? "" : "/", leaf);
	return (r > 0 && (size_t)r < size) ? buf : NULL;
}

// The cache file dyld_sim will take, as a Linux path in buf; NULL when it takes none, or mldr can't tell or read it.
static const char* cache_path(char* buf, size_t size)
{
	const char* root = getenv("DYLD_ROOT_PATH");
	if (!root)
		return NULL; // not a simulator process

	// first the runtime root's own cache, which DYLD_SHARED_REGION=avoid doesn't turn off
	char dir[4096];
	if (snprintf(dir, sizeof(dir), "%s/System/Library/Caches/com.apple.dyld/", root) >= (int)sizeof(dir))
		return NULL;
	const char* p = linux_path(buf, size, dir, CACHE_FILE);
	if (!p)
		return NULL; // a root elsewhere: whether it has a cache, so which one dyld_sim takes, isn't known
	if (access(p, F_OK) == 0)
		return p;

	const char* mode = getenv("DYLD_SHARED_REGION");
	if (mode && strcmp(mode, "avoid") == 0)
		return NULL;
	return linux_path(buf, size, getenv("DYLD_SHARED_CACHE_DIR"), CACHE_FILE);
}

// Adds the mappings of the open cache file fd (and, for the main file, its dynamic data page) to r, at the addresses
// dyld_sim maps them: base is the main file's first mapping's address, 0 for the main file itself. Returns the file's
// header in hdr, or -1 when the file isn't an x86_64 simulator cache dyld_sim would take.
static int add_file(int fd, unsigned char* hdr, struct range* r, int* n, uint64_t* base)
{
	if (pread(fd, hdr, H_SIZE, 0) != H_SIZE || memcmp(hdr, "dyld_v1  x86_64", 16) != 0)
		return -1;
	uint32_t moff = get32(hdr + H_MAPPING_OFFSET), mcount = get32(hdr + H_MAPPING_COUNT);
	if (moff < H_FLAGS + 4 || !(get32(hdr + H_FLAGS) & FLAG_SIMULATOR) || mcount == 0 || mcount > MAX_MAPPINGS)
		return -1;
	// dyld-1378 refuses a header from before sub-cache suffixes, a sub-cache's too
	if (moff <= H_CACHE_SUB_TYPE)
		return -1;
	unsigned char m[MAX_MAPPINGS * MAPPING_SIZE];
	if (pread(fd, m, mcount * MAPPING_SIZE, moff) != (ssize_t)(mcount * MAPPING_SIZE))
		return -1;
	int main_file = *base == 0;
	if (main_file && (*base = get64(m)) == 0)
		return -1;
	for (uint32_t i = 0; i < mcount; ++i) {
		uint64_t a = get64(m + i * MAPPING_SIZE), s = get64(m + i * MAPPING_SIZE + 8);
		// one dyld_sim couldn't map (below the first, or past the top of the address space): no cache
		if (a < *base || a - *base > USER_TOP - SHARED_REGION_BASE || s > USER_TOP - SHARED_REGION_BASE - (a - *base))
			return -1;
		if (s)
			r[(*n)++] = (struct range){ SHARED_REGION_BASE + (a - *base), SHARED_REGION_BASE + (a - *base) + s };
	}
	// the dynamic data page, at sharedRegionStart + dynamicDataOffset (SharedCacheRuntime.cpp, preflightCacheFile)
	if (main_file && moff >= H_DYNAMIC_DATA_OFFSET + 8 && get64(hdr + H_DYNAMIC_DATA_OFFSET)) {
		uint64_t a = get64(hdr + H_SHARED_REGION_START) + get64(hdr + H_DYNAMIC_DATA_OFFSET);
		r[(*n)++] = (struct range){ a, a + DYNAMIC_REGION_SIZE };
	}
	return 0;
}

// The ranges of the cache at `path`, its sub-caches' included (their file names are the main file's plus each
// entry's fileSuffix), sorted and merged. Returns how many, 0 for no cache dyld_sim would take.
static int cache_ranges(const char* path, struct range* r)
{
	unsigned char hdr[H_SIZE];
	uint64_t base = 0;
	int n = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	if (add_file(fd, hdr, r, &n, &base) < 0) {
		close(fd);
		return 0;
	}
	uint32_t soff = get32(hdr + H_SUBCACHE_ARRAY_OFFSET), scount = get32(hdr + H_SUBCACHE_ARRAY_COUNT);
	if (scount >= MAX_SUBCACHES) {
		close(fd);
		return 0;
	}
	static unsigned char e[MAX_SUBCACHES * SUBCACHE_ENTRY_SIZE];
	ssize_t want = (ssize_t)scount * SUBCACHE_ENTRY_SIZE;
	ssize_t got = want ? pread(fd, e, want, soff) : 0;
	close(fd);
	if (got != want)
		return 0;
	for (uint32_t i = 0; i < scount; ++i) {
		char suffix[33], sub[4096];
		unsigned char subhdr[H_SIZE];
		memcpy(suffix, e + i * SUBCACHE_ENTRY_SIZE + 24, 32);
		suffix[32] = '\0';
		if (snprintf(sub, sizeof(sub), "%s%s", path, suffix) >= (int)sizeof(sub))
			return 0;
		// dyld_sim refuses the whole cache when a sub-cache is missing or bad
		fd = open(sub, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return 0;
		int bad = add_file(fd, subhdr, r, &n, &base) < 0;
		close(fd);
		if (bad)
			return 0;
	}

	// sorted (insertion: a few dozen ranges, already almost in order), then merged where they touch or overlap
	for (int i = 1; i < n; ++i) {
		struct range x = r[i];
		int j = i;
		for (; j > 0 && r[j - 1].lo > x.lo; --j)
			r[j] = r[j - 1];
		r[j] = x;
	}
	int m = 0;
	for (int i = 0; i < n; ++i) {
		if (m && r[i].lo <= r[m - 1].hi) {
			if (r[i].hi > r[m - 1].hi)
				r[m - 1].hi = r[i].hi;
		} else
			r[m++] = r[i];
	}
	return m;
}

// Whether anything in /proc/self/maps comes within MARGIN of one of the ranges. Unreadable maps count as taken: it
// can't be told.
static int taken(const struct range* r, int n)
{
	int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 1;
	static char buf[1 << 16];
	size_t len = 0;
	ssize_t k;
	while (len < sizeof(buf) - 1 && (k = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0)
		len += k;
	close(fd);
	if (len == sizeof(buf) - 1)
		return 1;
	buf[len] = '\0';
	for (char* line = buf; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
		unsigned long a, b;
		if (sscanf(line, "%lx-%lx", &a, &b) != 2)
			continue;
		for (int i = 0; i < n; ++i) {
			if (a < r[i].hi + MARGIN && b + MARGIN > r[i].lo)
				return 1;
		}
	}
	return 0;
}

// what shared_region_reserve() reserved, for shared_region_reserved()
static struct range r[MAX_RANGES];
static int reserved_count;

int shared_region_reserved(unsigned long (*ranges)[2], int max)
{
	int n = reserved_count < max ? reserved_count : max;
	for (int i = 0; i < n; ++i) {
		ranges[i][0] = r[i].lo;
		ranges[i][1] = r[i].hi;
	}
	return reserved_count;
}

enum shared_region_result shared_region_reserve(unsigned long limit)
{
	char path[4096];
	int n;

	reserved_count = 0;

	if (!cache_path(path, sizeof(path)) || (n = cache_ranges(path, r)) == 0)
		return SHARED_REGION_OK;
	// a range that reaches the main thread's stack could never be mapped as dyld_sim does: say so, rather than
	// take layout after layout in vain
	if (r[n - 1].hi > limit) {
		fprintf(stderr, "mldr: the simulator's shared cache (%s) maps %#lx-%#lx, which runs into the main thread's "
			"stack: not reserved\n", path, r[n - 1].lo, r[n - 1].hi);
		return SHARED_REGION_OK;
	}
	if (taken(r, n))
		return SHARED_REGION_TAKEN;

	for (int i = 0; i < n; ++i) {
		void* want = (void*)r[i].lo;
		size_t len = r[i].hi - r[i].lo;
		void* p = mmap(want, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
		if (p == want)
			continue;
		int err = errno;
		// a kernel without MAP_FIXED_NOREPLACE (before 4.17) takes the address as a hint
		if (p != MAP_FAILED) {
			munmap(p, len);
			err = EEXIST;
		}
		// none of it stays: a part would only take address space from the process (under RLIMIT_AS, all it has left)
		for (int j = 0; j < i; ++j)
			munmap((void*)r[j].lo, r[j].hi - r[j].lo);
		// RLIMIT_AS or vm.max_map_count: dyld_sim's own mapping of the cache fails the same way, and it then runs
		// without the cache, as it would have without this. EEXIST can't be anything taken() missed (mldr runs alone
		// here, and the ranges don't overlap), so it isn't a reason for another layout either.
		fprintf(stderr, "mldr: can't reserve %#lx-%#lx for the simulator's shared cache (%s): %s\n", r[i].lo, r[i].hi,
			path, strerror(err));
		return SHARED_REGION_OK;
	}
	reserved_count = n;
	return SHARED_REGION_OK;
}

#endif // __x86_64__
