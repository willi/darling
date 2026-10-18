// getentropy() as on XNU (bsd/dev/random/randomdev.c): at most 256 bytes, more is EINVAL, as OpenBSD's getentropy()
// does. Darling passed any size to getrandom().
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <stdio.h>
#include <sys/random.h>

int main(void)
{
	unsigned char buf[257];

	errno = 0;
	int ok = getentropy(buf, sizeof(buf)) == -1 && errno == EINVAL;
	printf("%s - 257 bytes: EINVAL\n", ok ? "ok" : "not ok");
	return !ok;
}
