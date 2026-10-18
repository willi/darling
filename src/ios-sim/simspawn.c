/*
 * simspawn: runs a program in a booted iOS Simulator device, as `xcrun simctl spawn` does.
 *
 * Usage: SIM_RUNTIME_ROOT=<RuntimeRoot> simspawn <program> [args...]
 *
 * The device's launchd_sim spawns the program, so it runs in the device's bootstrap domain, with the environment
 * from the device's launchd_bootstrap.plist. `program` is relative to the runtime root unless it is absolute.
 * Variables named SIMCTL_CHILD_<name> are passed on as <name>. The program's stdio is simspawn's, and simspawn
 * exits with its status.
 *
 * Like CoreSimulator, this talks to launchd_sim through the runtime's host library, liblaunch_sim.dylib, whose
 * launch_sim_* functions look the device up by its launchd name in the host's bootstrap namespace. launchd_sim takes
 * the request only from a caller whose euid is its own uid, so run simspawn as the device's user (SIM_UID; through
 * simasuid from root), as CoreSimulator runs as the user who booted the device.
 */
#include <dlfcn.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <mach/machine.h>

#define DEFAULT_UDID "4C0A1A5E-0000-4000-8000-000000000001"
#define CHILD_PREFIX "SIMCTL_CHILD_"

extern char** environ;

typedef int (*launch_sim_spawn_f)(const char* launchd_name, const char* path, char* const* argv, char* const* envp,
		const char* unknown, int stdin_fd, int stdout_fd, int stderr_fd, cpu_type_t binpref, bool flag, pid_t* pid);
typedef int (*launch_sim_waitpid_f)(const char* launchd_name, pid_t pid, int* status);

static char** child_environment(void)
{
	size_t n = 0, i;
	char** env;

	for (i = 0; environ[i] != NULL; i++)
		n++;
	env = calloc(n + 1, sizeof(*env));
	if (env == NULL)
		return NULL;

	n = 0;
	for (i = 0; environ[i] != NULL; i++)
	{
		if (strncmp(environ[i], CHILD_PREFIX, sizeof(CHILD_PREFIX) - 1) == 0)
			env[n++] = environ[i] + sizeof(CHILD_PREFIX) - 1;
	}
	return env;
}

int main(int argc, char** argv)
{
	const char* root = getenv("SIM_RUNTIME_ROOT");
	const char* udid = getenv("SIM_UDID");
	char lib[PATH_MAX], path[PATH_MAX], name[128];
	launch_sim_spawn_f spawn;
	launch_sim_waitpid_f waitpid_sim;
	void* handle;
	pid_t pid;
	int status, err;
	char** env;

	if (argc < 2 || root == NULL)
	{
		fprintf(stderr, "usage: SIM_RUNTIME_ROOT=<RuntimeRoot> %s <program> [args...]\n", argv[0]);
		return 64;
	}

	snprintf(name, sizeof(name), "com.apple.CoreSimulator.SimDevice.%s", udid ? udid : DEFAULT_UDID);
	snprintf(lib, sizeof(lib), "%s/usr/lib/system/host/liblaunch_sim.dylib", root);
	if (argv[1][0] == '/')
		snprintf(path, sizeof(path), "%s", argv[1]);
	else
		snprintf(path, sizeof(path), "%s/%s", root, argv[1]);

	handle = dlopen(lib, RTLD_NOW);
	if (handle == NULL)
	{
		fprintf(stderr, "simspawn: %s\n", dlerror());
		return 69;
	}
	spawn = (launch_sim_spawn_f) dlsym(handle, "launch_sim_spawn");
	waitpid_sim = (launch_sim_waitpid_f) dlsym(handle, "launch_sim_waitpid");
	if (spawn == NULL || waitpid_sim == NULL)
	{
		fprintf(stderr, "simspawn: %s lacks launch_sim_spawn or launch_sim_waitpid\n", lib);
		return 69;
	}

	env = child_environment();
	if (env == NULL)
	{
		perror("simspawn");
		return 71;
	}

	err = spawn(name, path, argv + 1, env, NULL, 0, 1, 2, 0, false, &pid);
	if (err != 0)
	{
		fprintf(stderr, "simspawn: launch_sim_spawn(%s, %s): %d (%s); is the device booted?\n", name, path, err,
				strerror(err));
		return 69;
	}

	err = waitpid_sim(name, pid, &status);
	if (err != 0)
	{
		fprintf(stderr, "simspawn: launch_sim_waitpid(%d): %d\n", pid, err);
		return 70;
	}

	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return WEXITSTATUS(status);
}
