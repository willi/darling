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

enum shared_region_result shared_region_reserve(void);

#ifdef __cplusplus
}
#endif

#endif
