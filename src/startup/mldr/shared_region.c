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
// MAP_FIXED replaces the reservation.
//
// Which cache: the one dyld_sim's CacheFinder takes (dyld-1378 dyld/DyldProcessConfig.cpp), for a process with
// DYLD_ROOT_PATH: $DYLD_ROOT_PATH/System/Library/Caches/com.apple.dyld/ if the cache file is there, else
// $DYLD_SHARED_CACHE_DIR; none with DYLD_SHARED_REGION=avoid. Their paths are the process's (vchrooted) paths; before
// mldr checks in with darlingserver it doesn't know the prefix, so it reads only those under /Volumes/SystemRoot,
// which is the Linux root. A cache elsewhere isn't reserved for.

#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif
#include "shared_region.h"

// x86_64 only: mldr32 builds this file too, and a 32-bit process maps no simulator cache
#ifdef __x86_64__

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
#define SYSTEM_ROOT "/Volumes/SystemRoot/"

// dyld_cache_header fields (dyld-1378 include/mach-o/dyld_cache_format.h), by offset. A field exists only in a header
// that reaches past it: mappingOffset is the header's size.
#define H_MAPPING_OFFSET        0x10
#define H_MAPPING_COUNT         0x14
#define H_FLAGS                 0xdc
#define H_SUBCACHE_ARRAY_OFFSET 0x188
#define H_SUBCACHE_ARRAY_COUNT  0x18c
#define H_CACHE_SUB_TYPE        0x1c8 // a header past it has dyld_subcache_entry's fileSuffix
#define H_DYNAMIC_DATA_OFFSET   0x1f0
#define H_DYNAMIC_DATA_MAX_SIZE 0x1f8
#define H_SHARED_REGION_START   0xe0
#define H_SIZE                  0x200
#define FLAG_SIMULATOR          (1u << 9)
#define MAPPING_SIZE            32 // dyld_cache_mapping_info: address, size, fileOffset, maxProt, initProt
#define SUBCACHE_ENTRY_SIZE     56 // dyld_subcache_entry: uuid[16], cacheVMOffset, fileSuffix[32]
#define DYNAMIC_REGION_SIZE     0x4000 // DyldSharedCache::DynamicRegion::size()

#define MAX_FILES  32
#define MAX_RANGES 256

// the stack guard gap: no range comes within this of another mapping (the Linux stack grows down into it)
#define MARGIN 0x100000UL

// The top a range may reach: under mldr's stack below the commpage (0x7fffffe00000), its 64 KiB and the 9 MiB the
// Mach-O main thread's stack grows into. A cache reaching higher could never be mapped as dyld_sim does: mldr says so
// and leaves it, rather than redraw in vain.
#define RANGE_LIMIT (0x7fffffe00000UL - 0x10000UL - 0x900000UL)

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

// The Linux path of a process path under /Volumes/SystemRoot, with `leaf` appended (one "/" between them), in buf;
// NULL for any other path.
static const char* linux_path(char* buf, size_t size, const char* path, const char* leaf)
{
	if (!path || strncmp(path, SYSTEM_ROOT, sizeof(SYSTEM_ROOT) - 1) != 0)
		return NULL;
	path += sizeof(SYSTEM_ROOT) - 2; // keep the "/"
	size_t n = strlen(path);
	int r = snprintf(buf, size, "%s%s%s", path, (n && path[n - 1] == '/') ? "" : "/", leaf);
	return (r > 0 && (size_t)r < size) ? buf : NULL;
}

// The cache file dyld_sim will take, as a Linux path in buf; NULL when it takes none, or none this can read.
static const char* cache_path(char* buf, size_t size)
{
	const char* root = getenv("DYLD_ROOT_PATH");
	if (!root)
		root = getenv("__mldr_DYLD_ROOT_PATH");
	if (!root)
		return NULL; // not a simulator process
	const char* mode = getenv("DYLD_SHARED_REGION");
	if (mode && strcmp(mode, "avoid") == 0)
		return NULL;

	char dir[4096];
	if (snprintf(dir, sizeof(dir), "%s/System/Library/Caches/com.apple.dyld/", root) < (int)sizeof(dir)) {
		const char* p = linux_path(buf, size, dir, CACHE_FILE);
		if (p && access(p, F_OK) == 0)
			return p;
	}
	return linux_path(buf, size, getenv("DYLD_SHARED_CACHE_DIR"), CACHE_FILE);
}

// Adds the mappings of the cache file `path` (and, for the main file, its dynamic data page) to r. Returns the file's
// header in hdr, or -1 when the file isn't a readable x86_64 simulator cache.
static int add_file(const char* path, unsigned char* hdr, struct range* r, int* n, int main_file)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	int ok = -1;
	if (pread(fd, hdr, H_SIZE, 0) != H_SIZE || memcmp(hdr, "dyld_v1  x86_64", 16) != 0)
		goto out;
	uint32_t moff = get32(hdr + H_MAPPING_OFFSET), mcount = get32(hdr + H_MAPPING_COUNT);
	if (moff < H_FLAGS + 4 || !(get32(hdr + H_FLAGS) & FLAG_SIMULATOR) || mcount == 0 || mcount > 16)
		goto out;
	unsigned char m[16 * MAPPING_SIZE];
	if (pread(fd, m, mcount * MAPPING_SIZE, moff) != (ssize_t)(mcount * MAPPING_SIZE))
		goto out;
	for (uint32_t i = 0; i < mcount && *n < MAX_RANGES; ++i) {
		uint64_t a = get64(m + i * MAPPING_SIZE), s = get64(m + i * MAPPING_SIZE + 8);
		if (s)
			r[(*n)++] = (struct range){ a, a + s };
	}
	if (main_file && moff >= H_DYNAMIC_DATA_MAX_SIZE + 8 && *n < MAX_RANGES) {
		uint64_t a = get64(hdr + H_SHARED_REGION_START) + get64(hdr + H_DYNAMIC_DATA_OFFSET);
		uint64_t s = get64(hdr + H_DYNAMIC_DATA_MAX_SIZE);
		if (s < DYNAMIC_REGION_SIZE)
			s = DYNAMIC_REGION_SIZE;
		if (get64(hdr + H_DYNAMIC_DATA_OFFSET))
			r[(*n)++] = (struct range){ a, a + s };
	}
	ok = 0;
out:
	close(fd);
	return ok;
}

// The ranges of the cache at `path`, its sub-caches' included (their file names are the main file's plus each
// entry's fileSuffix). Returns how many, 0 for no usable cache.
static int cache_ranges(const char* path, struct range* r)
{
	unsigned char hdr[H_SIZE];
	int n = 0;
	if (add_file(path, hdr, r, &n, 1) < 0)
		return 0;
	uint32_t moff = get32(hdr + H_MAPPING_OFFSET);
	if (moff < H_SUBCACHE_ARRAY_COUNT + 4)
		return n;
	uint32_t soff = get32(hdr + H_SUBCACHE_ARRAY_OFFSET), scount = get32(hdr + H_SUBCACHE_ARRAY_COUNT);
	if (scount == 0)
		return n;
	// old sub-cache entries have no suffix: dyld-1378 refuses those caches, so there's nothing to reserve for
	if (moff <= H_CACHE_SUB_TYPE || scount > MAX_FILES)
		return 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	unsigned char e[MAX_FILES * SUBCACHE_ENTRY_SIZE];
	ssize_t want = (ssize_t)scount * SUBCACHE_ENTRY_SIZE;
	ssize_t got = pread(fd, e, want, soff);
	close(fd);
	if (got != want)
		return 0;
	for (uint32_t i = 0; i < scount; ++i) {
		char suffix[33], sub[4096], subhdr[H_SIZE];
		memcpy(suffix, e + i * SUBCACHE_ENTRY_SIZE + 24, 32);
		suffix[32] = '\0';
		if (snprintf(sub, sizeof(sub), "%s%s", path, suffix) >= (int)sizeof(sub))
			return 0;
		// dyld_sim refuses the whole cache when a sub-cache is missing or bad
		if (add_file(sub, (unsigned char*)subhdr, r, &n, 0) < 0)
			return 0;
	}
	return n;
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

enum shared_region_result shared_region_reserve(void)
{
	char path[4096];
	struct range r[MAX_RANGES];
	int n;

	if (!cache_path(path, sizeof(path)) || (n = cache_ranges(path, r)) == 0)
		return SHARED_REGION_OK;
	for (int i = 0; i < n; ++i) {
		if (r[i].hi < r[i].lo || r[i].hi > RANGE_LIMIT) {
			fprintf(stderr, "mldr: the simulator's shared cache (%s) maps %#lx-%#lx, which runs into mldr's stack: not "
				"reserved\n", path, r[i].lo, r[i].hi);
			return SHARED_REGION_OK;
		}
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
		if (err == EEXIST)
			return SHARED_REGION_TAKEN;
		// RLIMIT_AS or vm.max_map_count: dyld_sim's own mapping of the cache would likely fail the same way, and it
		// then runs without the cache
		fprintf(stderr, "mldr: can't reserve %#lx-%#lx for the simulator's shared cache (%s): %s\n", r[i].lo, r[i].hi,
			path, strerror(err));
		return SHARED_REGION_OK;
	}
	return SHARED_REGION_OK;
}

#endif // __x86_64__
