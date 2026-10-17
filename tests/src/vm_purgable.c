// mach_vm_purgable_control() as on XNU, for memory allocated with VM_FLAGS_PURGABLE. Darling had no trap 11
// (_kernelrpc_mach_vm_purgable_control_trap), so every call failed with KERN_FAILURE: CoreFoundation, QuartzCore,
// ImageIO, CoreData and others make their caches purgeable, and get an error back for each one.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_purgable.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

static kern_return_t get(mach_vm_address_t addr, int* state)
{
	*state = -1;
	return mach_vm_purgable_control(mach_task_self(), addr, VM_PURGABLE_GET_STATE, state);
}

// sets `state`, and gets back the one before
static kern_return_t set(mach_vm_address_t addr, int state, int* old)
{
	*old = state;
	return mach_vm_purgable_control(mach_task_self(), addr, VM_PURGABLE_SET_STATE, old);
}

static int state_is(mach_vm_address_t addr, int expected)
{
	int state;

	return get(addr, &state) == KERN_SUCCESS && state == expected;
}

static int zeros(const char* p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != 0)
			return 0;
	return 1;
}

// purgeable memory, filled with c; 0 if it can't be had
static mach_vm_address_t purgeable(size_t size, char c)
{
	mach_vm_address_t a = 0;

	if (mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE) != KERN_SUCCESS)
		return 0;
	memset((void*) a, c, size);
	return a;
}

int main(void)
{
	const size_t page = 4096, size = 4 * page;
	mach_vm_address_t a, b, c, plain = 0;
	int state, old;
	kern_return_t kr;

	setvbuf(stdout, NULL, _IOLBF, 0);

	a = purgeable(size, 'x');
	b = purgeable(size, 'y');
	check(a != 0 && b != 0, "purgeable memory can be allocated");
	if (a == 0 || b == 0)
		return failures;

	kr = get(a, &state);
	check(kr == KERN_SUCCESS && state == VM_PURGABLE_NONVOLATILE, "new purgeable memory is non-volatile");
	if (kr != KERN_SUCCESS)
		printf("# kr %d\n", kr);

	kr = set(a, VM_PURGABLE_VOLATILE, &old);
	check(kr == KERN_SUCCESS && old == VM_PURGABLE_NONVOLATILE && state_is(a + page + 12, VM_PURGABLE_VOLATILE),
	    "made volatile, from anywhere in it: the old state back, volatile now");

	kr = set(a, VM_PURGABLE_NONVOLATILE, &old);
	check(kr == KERN_SUCCESS && old == VM_PURGABLE_VOLATILE && ((char*) a)[size - 1] == 'x',
	    "non-volatile again: the old state back, the contents intact");

	kr = set(a, VM_PURGABLE_EMPTY, &old);
	check(kr == KERN_SUCCESS && old == VM_PURGABLE_NONVOLATILE && zeros((char*) a, size)
	    && state_is(a, VM_PURGABLE_EMPTY), "emptied: the contents are gone");

	// vm_object_purgable_control(): volatile memory that's empty stays empty
	kr = set(a, VM_PURGABLE_VOLATILE, &old);
	check(kr == KERN_SUCCESS && old == VM_PURGABLE_EMPTY && state_is(a, VM_PURGABLE_EMPTY),
	    "made volatile when empty: still empty");

	// VM_PURGABLE_PURGE_ALL empties volatile memory, and leaves the rest
	set(a, VM_PURGABLE_NONVOLATILE, &old);
	memset((void*) a, 'x', size);
	set(b, VM_PURGABLE_VOLATILE, &old);
	state = 0;
	kr = mach_vm_purgable_control(mach_task_self(), 0, VM_PURGABLE_PURGE_ALL, &state);
	check(kr == KERN_SUCCESS && zeros((char*) b, size) && state_is(b, VM_PURGABLE_EMPTY) && ((char*) a)[0] == 'x',
	    "purge all: the volatile memory emptied, the non-volatile kept");

	// VM_PURGABLE_DEBUG_EMPTY: from then on, volatile is empty at once
	c = purgeable(size, 'z');
	set(c, VM_PURGABLE_NONVOLATILE | VM_PURGABLE_DEBUG_EMPTY, &old);
	kr = set(c, VM_PURGABLE_VOLATILE, &old);
	check(kr == KERN_SUCCESS && state_is(c, VM_PURGABLE_EMPTY) && zeros((char*) c, size),
	    "VM_PURGABLE_DEBUG_EMPTY, then volatile: empty");

	// Regression: the fork child kept the table. On XNU, a fork child's copy is ordinary memory (vm_map_fork() copies
	// a purgeable object into a new one), and the parent's stays as it was.
	set(a, VM_PURGABLE_VOLATILE, &old);
	pid_t pid = fork();
	if (pid == 0)
		_exit(get(a, &state) == KERN_INVALID_ARGUMENT && ((char*) a)[0] == 'x' ? 0 : 1);
	int status;
	waitpid(pid, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0 && state_is(a, VM_PURGABLE_VOLATILE),
	    "in a fork child, the copy isn't purgeable; the parent's is still volatile");

	check(mach_vm_allocate(mach_task_self(), &plain, page, VM_FLAGS_ANYWHERE) == KERN_SUCCESS
	    && get(plain, &state) == KERN_INVALID_ARGUMENT, "memory that isn't purgeable: KERN_INVALID_ARGUMENT");

	// part of it unmapped: the rest is purgeable still, and the part has no mapping
	mach_vm_deallocate(mach_task_self(), b, page);
	check(get(b, &state) == KERN_INVALID_ADDRESS && get(b + page, &state) == KERN_SUCCESS,
	    "deallocated in part: KERN_INVALID_ADDRESS there, the rest purgeable still");

	// a hole in the middle: both sides are the one object still, with one state
	mach_vm_deallocate(mach_task_self(), a + page, page);
	set(a, VM_PURGABLE_NONVOLATILE, &old);
	check(state_is(a + 3 * page, VM_PURGABLE_NONVOLATILE), "a hole in the middle: one state for what's left");

	// mapped over (MAP_FIXED): not purgeable there
	void* over = mmap((void*) (b + page), page, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
	check(over != MAP_FAILED && get(b + page, &state) == KERN_INVALID_ARGUMENT && state_is(b + 2 * page,
	    VM_PURGABLE_EMPTY), "mapped over: not purgeable there, the rest purgeable still");

	state = VM_PURGABLE_NONVOLATILE;
	check(mach_vm_purgable_control(mach_task_self(), a, VM_PURGABLE_SET_STATE_FROM_KERNEL, &state)
	    == KERN_INVALID_ARGUMENT, "VM_PURGABLE_SET_STATE_FROM_KERNEL from user space: KERN_INVALID_ARGUMENT");
	check(set(a, VM_PURGABLE_DENY, &old) == KERN_INVALID_ARGUMENT, "VM_PURGABLE_DENY: KERN_INVALID_ARGUMENT");
	// XNU looks the address up first
	check(set(b, VM_PURGABLE_DENY, &old) == KERN_INVALID_ADDRESS, "VM_PURGABLE_DENY, no mapping: KERN_INVALID_ADDRESS");
	check(set(a, 1 << 30, &old) == KERN_INVALID_ARGUMENT, "a state with unknown bits: KERN_INVALID_ARGUMENT");

	return failures;
}
