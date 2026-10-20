// A process's unique ID (PROC_PIDUNIQIDENTIFIERINFO's p_uniqueid) and its parent's (p_puniqueid), as xnu keeps them:
// new at every fork, the same across exec, and the parent's for a child. darlingserver's task copied the ID before
// the registry assigned it, so a forked child read 0 or stale heap bytes, and a fork+exec's parent ID was wrong
// (taffy B39; darlingserver koala/01a145c-80f/fork-eid).
// The parent reads each child's IDs at once after fork, 1000 times, and after the child execs this binary, 50 times.
#define PRIVATE 1
#include <libproc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/wait.h>
#include <unistd.h>

#define FORKS 1000
#define EXECS 50

static int ids(pid_t pid, struct proc_uniqidentifierinfo* u)
{
	memset(u, 0, sizeof(*u));
	return proc_pidinfo(pid, PROC_PIDUNIQIDENTIFIERINFO, 0, u, sizeof(*u)) == sizeof(*u);
}

static int fails;
static void check(int n, const char* what)
{
	printf("%s - %s%s", n == 0 ? "ok" : "not ok", what, n == 0 ? "\n" : "");
	if (n)
	{
		printf(": %d of %d\n", n, FORKS + EXECS);
		fails++;
	}
}

int main(int argc, char** argv)
{
	static uint64_t seen[FORKS + EXECS];
	struct proc_uniqidentifierinfo self, u, v;
	int nseen = 0, failed = 0, zero = 0, dup = 0, badparent = 0, changed = 0;

	if (argc > 1 && strcmp(argv[1], "child") == 0)
	{
		char c;
		write(1, "r", 1); // exec'd: the parent may look now
		read(0, &c, 1);
		return 0;
	}
	if (!ids(getpid(), &self))
	{
		printf("not ok - PROC_PIDUNIQIDENTIFIERINFO on itself\n");
		return 1;
	}

	for (int i = 0; i < FORKS + EXECS; i++)
	{
		int exec = i >= FORKS, in[2], out[2];
		char c;
		if (pipe(in) || pipe(out))
			return 100;
		pid_t child = fork();
		if (child < 0)
			return 100;
		if (child == 0)
		{
			close(in[1]);
			close(out[0]);
			if (exec)
			{
				dup2(in[0], 0);
				dup2(out[1], 1);
				execl(argv[0], argv[0], "child", (char*) NULL);
				_exit(127);
			}
			read(in[0], &c, 1);
			_exit(0);
		}
		close(in[0]);
		close(out[1]);
		int got = ids(child, &u);
		if (exec && read(out[0], &c, 1) != 1)
			got = 0;
		if (got && exec)
			got = ids(child, &v);
		if (!got)
			failed++;
		else
		{
			struct proc_uniqidentifierinfo* now = exec ? &v : &u;
			if (u.p_uniqueid == 0 || now->p_uniqueid == 0)
				zero++;
			for (int k = 0; k < nseen; k++)
				if (seen[k] == now->p_uniqueid)
				{
					dup++;
					break;
				}
			seen[nseen++] = now->p_uniqueid;
			if (now->p_puniqueid != self.p_uniqueid)
				badparent++;
			if (exec && u.p_uniqueid != v.p_uniqueid)
				changed++;
		}
		close(in[1]);
		close(out[0]);
		waitpid(child, NULL, 0);
	}

	check(failed, "PROC_PIDUNIQIDENTIFIERINFO answers for every child");
	check(zero, "no child's unique ID is 0");
	check(dup, "no two children have the same unique ID");
	check(badparent, "every child's parent unique ID is its parent's");
	check(changed, "exec keeps the unique ID");
	return fails;
}
