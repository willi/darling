// mincore() takes any address, as on XNU: the pages the range touches are reported. Linux wants it page-aligned, and
// Apple's SQLite traps when mincore() fails on a tokenizer's function pointers (SpringBoard died of SIGILL).
//
// It only relies on XNU's interface, so it runs on macOS too. Prints one "ok"/"not ok" line per check; the exit status
// is the number of failed checks.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

int main(void)
{
	char vec[2] = { 0 };
	char* code = (char*) &check + 1;

	check(mincore(code, 8, vec) == 0 && (vec[0] & MINCORE_INCORE), "a function's address, not page-aligned");

	char* p = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	p[0] = p[4096] = 1;
	check(mincore(p + 4090, 12, vec) == 0 && (vec[0] & MINCORE_INCORE) && (vec[1] & MINCORE_INCORE),
		"a range across two pages reports both");

	munmap(p, 2 * 4096);
	errno = 0;
	check(mincore(p + 10, 8, vec) == -1 && errno == ENOMEM, "an unmapped address is ENOMEM");
	return failures;
}
