// memorystatus_control() answers as macOS's XNU does: it keeps what a process's jetsam priority, user data, memory
// limits and "managed" flag are set to, for that process only, until it exits, and refuses the commands of the jetsam
// and the freezer macOS lacks with EINVAL. RunningBoard sets them for every process it launches, and treats only ESRCH
// and ENOTSUP as expected; there was no such syscall (ENOSYS).
// On macOS it takes root (or an entitlement); the test says so rather than failing.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/kern_memorystatus.h>
#include <sys/wait.h>
#include <unistd.h>

// newer than some SDKs' kern_memorystatus.h
#ifndef MEMORYSTATUS_CMD_MARK_PROCESS_COALITION_SWAPPABLE
#define MEMORYSTATUS_CMD_MARK_PROCESS_COALITION_SWAPPABLE 25
#endif
#ifndef MEMORYSTATUS_CMD_CONVERT_MEMLIMIT_MB
#define MEMORYSTATUS_CMD_CONVERT_MEMLIMIT_MB 28
#endif
#ifndef MEMORYSTATUS_CMD_GRP_SET_PROPERTIES
#define MEMORYSTATUS_CMD_GRP_SET_PROPERTIES 100
#endif
#ifndef kMemorystatusAssertion
#define kMemorystatusAssertion 0x40
#endif
// the bands of xnu-10002 (macOS 14), which a simulator runs on; older SDKs' headers have bands a tenth of these
#undef JETSAM_PRIORITY_DEFAULT
#undef JETSAM_PRIORITY_FOREGROUND
#undef JETSAM_PRIORITY_MAX
#define JETSAM_PRIORITY_DEFAULT 180
#define JETSAM_PRIORITY_FOREGROUND 100
#define JETSAM_PRIORITY_MAX 210

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	fflush(stdout);
	if (!ok)
		failures++;
}

static int is_managed(pid_t pid)
{
	return memorystatus_control(MEMORYSTATUS_CMD_GET_PROCESS_IS_MANAGED, pid, 0, NULL, 0);
}

static int set_priority(pid_t pid, int32_t priority, uint64_t user_data, uint32_t flags)
{
	memorystatus_priority_properties_t properties = { .priority = priority, .user_data = user_data };
	return memorystatus_control(MEMORYSTATUS_CMD_SET_PRIORITY_PROPERTIES, pid, flags, &properties, sizeof(properties));
}

// the process's entry, or one with pid -1
static memorystatus_priority_entry_t priority_entry(pid_t pid)
{
	memorystatus_priority_entry_t entry;
	if (memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_LIST, pid, 0, &entry, sizeof(entry)) != sizeof(entry))
		entry.pid = -1;
	return entry;
}

int main(void)
{
	pid_t self = getpid(), child;
	memorystatus_priority_entry_t entry;
	memorystatus_memlimit_properties_t limits;
	int pipefd[2], status, size;
	char byte;

	if (memorystatus_control(MEMORYSTATUS_CMD_SET_PROCESS_IS_MANAGED, self, 1, NULL, 0) != 0 && errno == EPERM)
	{
		printf("ok - # SKIP memorystatus_control() takes root or an entitlement\n");
		return 0;
	}
	check(is_managed(self) == 1, "a process set managed reads back as managed");
	memorystatus_control(MEMORYSTATUS_CMD_SET_PROCESS_IS_MANAGED, self, 0, NULL, 0);
	check(is_managed(self) == 0, "and as unmanaged once unset");

	check(set_priority(self, 100, 0xfeed, 0) == 0, "setting a priority and user data");
	entry = priority_entry(self);
	check(entry.pid == self && entry.priority == 100 && entry.user_data == 0xfeed && entry.limit == -1,
		"they read back, with no memory limit (-1)");
	set_priority(self, -1, 0, 0);
	check(priority_entry(self).priority == JETSAM_PRIORITY_DEFAULT, "a priority of -1 is the default one");
	check(set_priority(self, JETSAM_PRIORITY_MAX + 1, 0, 0) == -1 && errno == EINVAL, "one above the highest is EINVAL");

	// RunningBoard's: an assertion's priority, then its end, which a move to idle is
	set_priority(self, JETSAM_PRIORITY_FOREGROUND, 0, MEMORYSTATUS_SET_PRIORITY_ASSERTION);
	entry = priority_entry(self);
	check(entry.priority == JETSAM_PRIORITY_FOREGROUND && (entry.state & kMemorystatusAssertion),
		"an assertion's priority applies, and the process is under assertion");
	set_priority(self, JETSAM_PRIORITY_IDLE, 0, MEMORYSTATUS_SET_PRIORITY_ASSERTION);
	entry = priority_entry(self);
	check(entry.priority == JETSAM_PRIORITY_IDLE && !(entry.state & kMemorystatusAssertion),
		"moving it to idle ends the assertion");

	limits = (memorystatus_memlimit_properties_t) { 200, MEMORYSTATUS_MEMLIMIT_ATTR_FATAL, 100, 0 };
	memorystatus_control(MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES, self, 0, &limits, sizeof(limits));
	memset(&limits, 0, sizeof(limits));
	memorystatus_control(MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES, self, 0, &limits, sizeof(limits));
	check(limits.memlimit_active == 200 && limits.memlimit_active_attr == MEMORYSTATUS_MEMLIMIT_ATTR_FATAL
		&& limits.memlimit_inactive == 100 && limits.memlimit_inactive_attr == 0, "memory limits read back");
	limits = (memorystatus_memlimit_properties_t) { 0, 0, -5, 0 };
	memorystatus_control(MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES, self, 0, &limits, sizeof(limits));
	memorystatus_control(MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES, self, 0, &limits, sizeof(limits));
	check(limits.memlimit_active == -1 && limits.memlimit_active_attr == MEMORYSTATUS_MEMLIMIT_ATTR_FATAL
		&& limits.memlimit_inactive == -1 && limits.memlimit_inactive_attr == MEMORYSTATUS_MEMLIMIT_ATTR_FATAL,
		"limits of 0 or less are none, which are fatal");

	// the whole list: its size, then every process, by priority
	size = memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_LIST, 0, 0, NULL, 0);
	check(size > 0 && size % sizeof(memorystatus_priority_entry_t) == 0, "the list's size comes in entries");
	if (size > 0)
	{
		memorystatus_priority_entry_t list[size / sizeof(memorystatus_priority_entry_t) + 16];
		int count = memorystatus_control(MEMORYSTATUS_CMD_GET_PRIORITY_LIST, 0, 0, list, sizeof(list))
			/ (int) sizeof(*list);
		int found = 0, sorted = 1;

		for (int i = 0; i < count; i++)
		{
			found |= list[i].pid == self;
			sorted &= i == 0 || list[i - 1].priority <= list[i].priority;
		}
		check(found && sorted, "the list has this process, and goes by priority");
	}

	// another process's state is its own: a fork child starts at the default priority, unmanaged
	set_priority(self, 50, 0, 0);
	memorystatus_control(MEMORYSTATUS_CMD_SET_PROCESS_IS_MANAGED, self, 1, NULL, 0);
	pipe(pipefd);
	child = fork();
	if (child == 0)
	{
		read(pipefd[0], &byte, 1);
		_exit(0);
	}
	entry = priority_entry(child);
	check(entry.pid == child && entry.priority == JETSAM_PRIORITY_DEFAULT && is_managed(child) == 0,
		"a fork child starts at the default priority, unmanaged");
	memorystatus_control(MEMORYSTATUS_CMD_SET_PROCESS_IS_MANAGED, child, 1, NULL, 0);
	set_priority(child, JETSAM_PRIORITY_FOREGROUND, 0, 0);
	check(is_managed(child) == 1 && priority_entry(child).priority == JETSAM_PRIORITY_FOREGROUND
		&& priority_entry(self).priority == 50, "what's set for the child is the child's alone");
	write(pipefd[1], "", 1);
	waitpid(child, &status, 0);
	// darlingserver learns of the exit soon after, not at once
	for (int i = 0; i < 1000 && is_managed(child) != -1; i++)
		usleep(10000);
	check(is_managed(child) == -1 && errno == ESRCH, "and the child's goes when it exits (ESRCH)");

	// macOS's XNU has memorystatus without CONFIG_JETSAM or CONFIG_FREEZE
	check(memorystatus_control(MEMORYSTATUS_CMD_MARK_PROCESS_COALITION_SWAPPABLE, self, 0, NULL, 0) == -1
		&& errno == EINVAL, "jetsam's commands (coalition swappability) are EINVAL");
	check(memorystatus_control(MEMORYSTATUS_CMD_CONVERT_MEMLIMIT_MB, self, (uint32_t) -1, NULL, 0) == -1
		&& errno == EINVAL, "and memory limit conversion");
	check(memorystatus_control(MEMORYSTATUS_CMD_SET_PROCESS_IS_FREEZABLE, self, 0, NULL, 0) == -1 && errno == EINVAL,
		"the freezer's are EINVAL");

	check(memorystatus_control(MEMORYSTATUS_CMD_GET_PRESSURE_STATUS, 0, 0, NULL, 0) == 0,
		"memory pressure is normal");

	// BSD errnos: macOS refuses group properties with no group flag (EINVAL); Darling doesn't have them yet (ENOTSUP, 45:
	// not Linux's 95, which is EMULTIHOP here)
	check(memorystatus_control(MEMORYSTATUS_CMD_GRP_SET_PROPERTIES, 0, 0, NULL, 0) == -1 && (errno == EINVAL || errno == ENOTSUP),
		"group properties with no group flag fail with a BSD errno");

	return failures;
}
