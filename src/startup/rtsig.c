#include <signal.h>
#include <stdio.h>

// Determine RT signal ranges without polluting the build environment with Linux system headers
int main(int argc, char** argv)
{
	FILE* output = stdout;

	if (argc > 1)
	{
		output = fopen(argv[1], "w");
		if (!output)
		{
			perror("fopen");
			return 1;
		}
	}

	fprintf(output, "#define LINUX_SIGRTMIN %d\n", SIGRTMIN);
	fprintf(output, "#define LINUX_SIGRTMAX %d\n", SIGRTMAX);

	// Darling's own real-time signals, the first three: libsystem_kernel handles them (sigexc.h), darlingserver sends
	// them. Each needs a signal of its own.
	if (SIGRTMIN + 2 > SIGRTMAX)
	{
		fprintf(stderr, "rtsig: Darling needs three real-time signals, and %d..%d has fewer\n", SIGRTMIN, SIGRTMAX);
		return 1;
	}
	// the debugger's suspend (PT_SIGEXC)
	fprintf(output, "#define SIGNAL_SIGEXC_SUSPEND LINUX_SIGRTMIN\n");
	// a server-to-client call (S2C)
	fprintf(output, "#define SIGNAL_S2C (LINUX_SIGRTMIN + 1)\n");
	// pthread_markcancel's: the target thread marks itself canceled (bsdthread/cancelable.h)
	fprintf(output, "#define SIGNAL_PTHREAD_CANCEL (LINUX_SIGRTMIN + 2)\n");

	fclose(output);
	return 0;
}

