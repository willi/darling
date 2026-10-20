// A non-root process's bootstrap calls get an answer from Darling's launchd, not a referral to a per-user launchd that
// never answers (B54). launchd 842 sends an anonymous client whose audit token has a non-zero ruid and euid to a
// per-user launchd (VPROC_ERR_TRY_PER_USER from job_mig_look_up2(), job_mig_register2() and job_mig_spawn_internal()).
// In Darling nothing serves the per-user bootstrap port the client then gets, so once darlingserver gave the token a
// real ruid (darlingserver#18), `id` and `defaults` run as 501 (simasuid 501 20 ...) hung at their first lookup.
//
// Run as root. The parent registers a name as root. The child becomes uid 501 gid 20 and, each step under a 20 s alarm:
// looks that name up; tries to register over it (refused: a non-root client can't take a name another job holds);
// registers and looks up a name of its own; asks launchd to spawn a job (refused, at once). Names of the test's own
// rather than notifyd's: a bare prefix may run no notifyd, and launchd referred the client before it looked at the
// name. This pins that the calls are answered, not which namespace a non-root registration lands in (the shared root
// one, as in launchd's embedded build; a Mac would put it in the user's own domain).
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <bootstrap.h>
#include <errno.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// liblaunch's launch_priv.h, which isn't in the SDK's headers
pid_t _spawn_via_launchd(const char* label, const char* const* argv, const void* spawn_attrs, int struct_version);

static int failures;
static const char* step;

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
	static const char head[] = "not ok - as uid 501, still waiting after 20 s: ";
	write(STDOUT_FILENO, head, sizeof(head) - 1);
	write(STDOUT_FILENO, step, strlen(step));
	write(STDOUT_FILENO, "\n", 1);
	_exit(100);
}

static mach_port_t new_port(void)
{
	mach_port_t port = MACH_PORT_NULL;
	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
	mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
	return port;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static kern_return_t register_name(const char* name, mach_port_t port)
{
	return bootstrap_register(bootstrap_port, (char*) name, port);
}
#pragma clang diagnostic pop

static int as_501(const char* root_name)
{
	mach_port_t port = MACH_PORT_NULL;
	char name[64];
	kern_return_t kr;

	if (setgid(20) != 0 || setuid(501) != 0 || getuid() != 501) {
		check(0, "became uid 501 gid 20");
		return failures;
	}
	signal(SIGALRM, timed_out);

	step = "look up a name root registered";
	alarm(20);
	kr = bootstrap_look_up(bootstrap_port, root_name, &port);
	alarm(0);
	check(kr == KERN_SUCCESS && MACH_PORT_VALID(port), "as uid 501: look up a name root registered");

	step = "register over root's name";
	alarm(20);
	kr = register_name(root_name, new_port());
	alarm(0);
	check(kr == BOOTSTRAP_NOT_PRIVILEGED, "as uid 501: registering over root's name is refused");

	mach_port_t recv = new_port();
	snprintf(name, sizeof(name), "org.darlinghq.tests.peruser_bootstrap.%d", getpid());
	step = "register a name";
	alarm(20);
	kr = register_name(name, recv);
	alarm(0);
	check(kr == KERN_SUCCESS, "as uid 501: register a name");

	port = MACH_PORT_NULL;
	step = "look up the name it registered";
	alarm(20);
	kr = bootstrap_look_up(bootstrap_port, name, &port);
	alarm(0);
	check(kr == KERN_SUCCESS && port == recv, "as uid 501: look up the name it registered");

	const char* const argv[] = { "/usr/bin/true", NULL };
	snprintf(name, sizeof(name), "org.darlinghq.tests.peruser_bootstrap.spawn.%d", getpid());
	step = "spawn a job";
	alarm(20);
	errno = 0;
	pid_t pid = _spawn_via_launchd(name, argv, NULL, 3);
	int err = errno;
	alarm(0);
	check(pid < 0 && err == EPERM, "as uid 501: a spawn through launchd is refused");
	return failures;
}

int main(void)
{
	if (getuid() != 0) {
		printf("not ok - run as root\n");
		return 1;
	}
	// registered before the fork, so the name is in the bootstrap the child looks in
	char root_name[64];
	snprintf(root_name, sizeof(root_name), "org.darlinghq.tests.peruser_bootstrap.root.%d", getpid());
	kern_return_t kr = register_name(root_name, new_port());
	check(kr == KERN_SUCCESS, "as root: register a name");
	if (kr != KERN_SUCCESS)
		return failures;

	pid_t pid = fork();
	if (pid == 0)
		_exit(as_501(root_name));
	int status = 0;
	if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
		check(0, "the uid 501 child exited");
		return failures;
	}
	return WEXITSTATUS(status) == 100 ? 1 : WEXITSTATUS(status);
}
