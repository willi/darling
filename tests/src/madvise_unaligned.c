// madvise() of a range that doesn't start on a page succeeds, as on XNU (which advises the pages the range touches),
// and leaves the memory as it was. Linux refuses such an address with EINVAL; LaunchServices' prewarming
// (MADV_WILLNEED on a property list inside its database) failed that way thousands of times per data migration.
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

int main(void)
{
	size_t page = getpagesize();
	char* p = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

	check(p != MAP_FAILED, "mmap two pages");
	if (p == MAP_FAILED)
		return failures;
	memset(p, 'x', 2 * page);

	check(madvise(p + 100, page, MADV_WILLNEED) == 0, "MADV_WILLNEED on an unaligned range");
	check(madvise(p + 100, 10, MADV_NORMAL) == 0, "MADV_NORMAL on an unaligned range");
	check(madvise(p + page - 1, 2, MADV_SEQUENTIAL) == 0, "MADV_SEQUENTIAL across a page boundary");

	int intact = 1;
	for (size_t i = 0; i < 2 * page; i++)
		intact &= p[i] == 'x';
	check(intact, "the memory is unchanged");

	return failures;
}
