#ifndef _MLDR_SHARED_REGION_H
#define _MLDR_SHARED_REGION_H

#ifdef __cplusplus
extern "C" {
#endif

// What shared_region_reserve() found.
enum shared_region_result {
	// nothing to keep free (no simulator cache for this process), or its ranges are reserved
	SHARED_REGION_OK,
	// something is already in a range the cache maps: the caller takes another address space layout
	SHARED_REGION_TAKEN,
};

// For a process that will map the iOS Simulator's dyld shared cache (shared_region.c): TAKEN when something is in its
// ranges, else OK, with the ranges reserved. limit: the lowest address of the main thread's stack, as far as it may
// grow; a cache reaching above it isn't reserved for.
enum shared_region_result shared_region_reserve(unsigned long limit);

#ifdef __cplusplus
}
#endif

#endif
