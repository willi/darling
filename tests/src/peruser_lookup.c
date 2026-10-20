// A non-root process's bootstrap calls are served by Darling's launchd from the system domain (B54). launchd 842
// sends an anonymous client whose audit token has a non-zero ruid and euid to a per-user launchd
// (VPROC_ERR_TRY_PER_USER from job_mig_look_up2() and job_mig_register2()), and Darling runs none, so the client then
// waited forever on a per-user bootstrap port: since darlingserver gives the token a real ruid (B33), `id` and
// `defaults` run as 501 (simasuid 501 20 ...) hung at their first lookup, com.apple.system.notification_center.
//
// Run as root. The child becomes uid 501 gid 20 and looks up notifyd's service and registers a name, each under a
// 20 s alarm. Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <bootstrap.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	fflush(stdout);
	if (!ok)
		failures++;
}

static void timed_out(int sig)
{
	(void) sig;
	static const char msg[] = "not ok - a bootstrap call as uid 501 still waiting after 20 s\n";
	write(STDOUT_FILENO, msg, sizeof(msg) - 1);
	_exit(100);
}

static int as_501(void)
{
	mach_port_t port = MACH_PORT_NULL;
	char name[64];

	if (setgid(20) != 0 || setuid(501) != 0 || getuid() != 501) {
		check(0, "became uid 501 gid 20");
		return failures;
	}
	signal(SIGALRM, timed_out);

	alarm(20);
	kern_return_t kr = bootstrap_look_up(bootstrap_port, "com.apple.system.notification_center", &port);
	alarm(0);
	check(kr == KERN_SUCCESS && MACH_PORT_VALID(port), "as uid 501: look up com.apple.system.notification_center");

	mach_port_t recv = MACH_PORT_NULL;
	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &recv);
	mach_port_insert_right(mach_task_self(), recv, recv, MACH_MSG_TYPE_MAKE_SEND);
	snprintf(name, sizeof(name), "org.darlinghq.tests.peruser_lookup.%d", getpid());
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	alarm(20);
	kr = bootstrap_register(bootstrap_port, name, recv);
	alarm(0);
#pragma clang diagnostic pop
	check(kr == KERN_SUCCESS, "as uid 501: register a name");

	port = MACH_PORT_NULL;
	alarm(20);
	kr = bootstrap_look_up(bootstrap_port, name, &port);
	alarm(0);
	check(kr == KERN_SUCCESS && port == recv, "as uid 501: look up the name it registered");
	return failures;
}

int main(void)
{
	if (getuid() != 0) {
		printf("not ok - run as root\n");
		return 1;
	}
	pid_t pid = fork();
	if (pid == 0)
		_exit(as_501());
	int status = 0;
	if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
		check(0, "the uid 501 child exited");
		return failures;
	}
	return WEXITSTATUS(status) == 100 ? 1 : WEXITSTATUS(status);
}
