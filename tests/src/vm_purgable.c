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

static int zeros(const char* p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != 0)
			return 0;
	return 1;
}

int main(void)
{
	const size_t size = 4 * 4096;
	mach_vm_address_t a = 0, b = 0, plain = 0;
	int state, old;
	kern_return_t kr;

	setvbuf(stdout, NULL, _IOLBF, 0);

	mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE);
	memset((void*) a, 'x', size);

	kr = get(a, &state);
	check(kr == KERN_SUCCESS && state == VM_PURGABLE_NONVOLATILE, "new purgeable memory is non-volatile");
	if (kr != KERN_SUCCESS)
		printf("# kr %d\n", kr);

	kr = set(a, VM_PURGABLE_VOLATILE, &old);
	check(kr == KERN_SUCCESS && old == VM_PURGABLE_NONVOLATILE && get(a + 4096 + 12, &state) == KERN_SUCCESS
	    && state == VM_PURGABLE_VOLATILE, "made volatile, from anywhere in it: the old state back, volatile now");

	kr = set(a, VM_PURGABLE_NONVOLATILE, &old);
	check(kr == KERN_SUCCESS && old == VM_PURGABLE_VOLATILE && ((char*) a)[size - 1] == 'x',
	    "non-volatile again: the old state back, the contents intact");

	kr = set(a, VM_PURGABLE_EMPTY, &old);
	check(kr == KERN_SUCCESS && old == VM_PURGABLE_NONVOLATILE && zeros((char*) a, size)
	    && get(a, &state) == KERN_SUCCESS && state == VM_PURGABLE_EMPTY, "emptied: the contents are gone");

	// vm_object_purgable_control(): volatile memory that's empty stays empty
	kr = set(a, VM_PURGABLE_VOLATILE, &old);
	check(kr == KERN_SUCCESS && old == VM_PURGABLE_EMPTY && get(a, &state) == KERN_SUCCESS
	    && state == VM_PURGABLE_EMPTY, "made volatile when empty: still empty");

	// VM_PURGABLE_PURGE_ALL empties volatile memory, and leaves the rest
	set(a, VM_PURGABLE_NONVOLATILE, &old);
	memset((void*) a, 'x', size);
	mach_vm_allocate(mach_task_self(), &b, size, VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE);
	memset((void*) b, 'y', size);
	set(b, VM_PURGABLE_VOLATILE, &old);
	state = 0;
	kr = mach_vm_purgable_control(mach_task_self(), 0, VM_PURGABLE_PURGE_ALL, &state);
	check(kr == KERN_SUCCESS && zeros((char*) b, size) && get(b, &state) == KERN_SUCCESS
	    && state == VM_PURGABLE_EMPTY && ((char*) a)[0] == 'x',
	    "purge all: the volatile memory emptied, the non-volatile kept");

	// a fork child's memory is a copy, purgeable still
	set(a, VM_PURGABLE_VOLATILE, &old);
	pid_t pid = fork();
	if (pid == 0)
		_exit(get(a, &state) == KERN_SUCCESS && state == VM_PURGABLE_VOLATILE ? 0 : 1);
	int status;
	waitpid(pid, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "in a fork child, purgeable and volatile still");

	mach_vm_allocate(mach_task_self(), &plain, 4096, VM_FLAGS_ANYWHERE);
	check(get(plain, &state) == KERN_INVALID_ARGUMENT, "memory that isn't purgeable: KERN_INVALID_ARGUMENT");

	// part of it unmapped: the rest is purgeable still, and the part has no mapping
	mach_vm_deallocate(mach_task_self(), b, 4096);
	check(get(b, &state) == KERN_INVALID_ADDRESS && get(b + 4096, &state) == KERN_SUCCESS,
	    "deallocated: KERN_INVALID_ADDRESS there, the rest purgeable still");

	state = VM_PURGABLE_NONVOLATILE;
	check(mach_vm_purgable_control(mach_task_self(), a, VM_PURGABLE_SET_STATE_FROM_KERNEL, &state)
	    == KERN_INVALID_ARGUMENT, "VM_PURGABLE_SET_STATE_FROM_KERNEL from user space: KERN_INVALID_ARGUMENT");
	check(set(a, VM_PURGABLE_DENY, &old) == KERN_INVALID_ARGUMENT, "VM_PURGABLE_DENY: KERN_INVALID_ARGUMENT");
	check(set(a, 1 << 30, &old) == KERN_INVALID_ARGUMENT, "a state with unknown bits: KERN_INVALID_ARGUMENT");

	return failures;
}
