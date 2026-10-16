// madvise() of a range that doesn't start on a page succeeds, as on XNU (which advises the pages the range touches).
// Linux refuses such an address with EINVAL; LaunchServices' prewarming (MADV_WILLNEED on a property list inside its
// database) failed that way thousands of times per data migration. The widening stays within the pages the range
// touches, and never applies to MADV_DONTNEED, which Linux carries out by discarding private pages.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

static int filled(const char* p, size_t len, char c)
{
	for (size_t i = 0; i < len; i++) {
		if (p[i] != c)
			return 0;
	}
	return 1;
}

int main(void)
{
	size_t page = getpagesize();
	// three pages, the third unmapped again: an advice that reached past the second would fail with ENOMEM
	char* p = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

	// each line out before a crash can lose it
	setvbuf(stdout, NULL, _IOLBF, 0);
	check(p != MAP_FAILED && munmap(p + 2 * page, page) == 0, "map two pages followed by a hole");
	if (p == MAP_FAILED)
		return failures;
	memset(p, 'x', 2 * page);

	check(madvise(p + 100, page, MADV_WILLNEED) == 0, "MADV_WILLNEED on an unaligned range");
	check(madvise(p + 100, 10, MADV_NORMAL) == 0, "MADV_NORMAL on an unaligned range");
	check(madvise(p + page - 1, 2, MADV_SEQUENTIAL) == 0, "MADV_SEQUENTIAL across a page boundary");
	check(madvise(p + 2 * page - 1, 1, MADV_WILLNEED) == 0, "the last byte before the hole: no advice past it");
	check(madvise(p + 100, (size_t) -50, MADV_WILLNEED) == -1 && errno == EINVAL,
		"a range that wraps past the end of memory fails with EINVAL");

	// XNU's MADV_DONTNEED is a hint that keeps the contents; an unaligned one must not become Linux's discard of the
	// whole page (whatever it returns)
	madvise(p + 100, 10, MADV_DONTNEED);
	check(filled(p, 2 * page, 'x'), "an unaligned MADV_DONTNEED leaves both pages' contents");

	return failures;
}
