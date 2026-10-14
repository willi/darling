// Mach memory entries as XNU has them, which the os_log transport is built on: a process makes a memory entry for its
// firehose buffer (MAP_MEM_VM_SHARE) and sends it to logd, which maps it read-only and reads what the process logs.
// Each mapping here comes from a memory entry, so on Darling it maps the entry's memfd as logd would. It only relies on
// XNU's interface, so it runs on macOS too.
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(bool ok, const char* name, const char* detail)
{
	if (ok)
		printf("ok - %s\n", name);
	else
	{
		printf("not ok - %s: %s\n", name, detail);
		failures++;
	}
	fflush(stdout);
}

static mach_vm_address_t allocate(mach_vm_size_t size)
{
	mach_vm_address_t address = 0;

	if (mach_vm_allocate(mach_task_self(), &address, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
		return 0;
	return address;
}

static kern_return_t make_entry(mach_vm_address_t address, mach_vm_size_t size, vm_prot_t permission, mach_port_t* entry)
{
	memory_object_size_t entry_size = size;

	*entry = MACH_PORT_NULL;
	return mach_make_memory_entry_64(mach_task_self(), &entry_size, address, permission, entry, MACH_PORT_NULL);
}

static kern_return_t map_entry(mach_port_t entry, mach_vm_size_t size, vm_prot_t protection, mach_vm_address_t* address)
{
	*address = 0;
	return mach_vm_map(mach_task_self(), address, size, 0, VM_FLAGS_ANYWHERE, entry, 0, FALSE, protection, protection,
		VM_INHERIT_NONE);
}

int main(void)
{
	const mach_vm_size_t size = 4 * vm_page_size;
	const vm_prot_t rw = VM_PROT_READ | VM_PROT_WRITE;
	mach_vm_address_t buffer, view, other, copy;
	mach_port_t entry, again;
	kern_return_t kr;

	// the firehose: a buffer the process keeps writing to, shared once, then mapped read-only by logd
	buffer = allocate(size);
	strcpy((char*) buffer, "written before sharing");
	kr = make_entry(buffer, size, MAP_MEM_VM_SHARE | VM_PROT_READ, &entry);
	check(kr == KERN_SUCCESS, "share a buffer", mach_error_string(kr));
	kr = map_entry(entry, size, VM_PROT_READ, &view);
	check(kr == KERN_SUCCESS, "map the shared buffer read-only", mach_error_string(kr));
	if (kr == KERN_SUCCESS)
	{
		check(strcmp((char*) view, "written before sharing") == 0, "the mapping has what was there before",
			(char*) view);
		strcpy((char*) buffer + vm_page_size * 3, "written after sharing");
		check(strcmp((char*) view + vm_page_size * 3, "written after sharing") == 0,
			"the mapping sees what's written to the buffer after", (char*) view + vm_page_size * 3);
	}

	// a mapping for at most the entry's protection, and it stays that way
	kr = map_entry(entry, size, rw, &other);
	check(kr == KERN_INVALID_RIGHT, "a read-only entry can't be mapped writable", mach_error_string(kr));
	kr = mach_vm_protect(mach_task_self(), view, size, FALSE, rw);
	check(kr != KERN_SUCCESS, "a read-only mapping can't be made writable", "it was");

	// libdispatch shares its buffer again when it reconnects to logd; both entries must be the same memory
	kr = make_entry(buffer, size, MAP_MEM_VM_SHARE | rw, &again);
	check(kr == KERN_SUCCESS, "share the buffer again", mach_error_string(kr));
	kr = map_entry(again, size, rw, &other);
	if (kr == KERN_SUCCESS)
	{
		strcpy((char*) other, "written through the second entry");
		check(strcmp((char*) view, "written through the second entry") == 0 &&
				strcmp((char*) buffer, "written through the second entry") == 0,
			"both entries are the buffer", (char*) view);
	}
	else
		check(false, "both entries are the buffer", mach_error_string(kr));

	// the mappings outlive the entries
	mach_port_deallocate(mach_task_self(), entry);
	mach_port_deallocate(mach_task_self(), again);
	strcpy((char*) buffer, "written after the entries are gone");
	check(strcmp((char*) view, "written after the entries are gone") == 0, "mappings outlive their entry",
		(char*) view);

	// MAP_MEM_VM_COPY: a snapshot
	kr = make_entry(buffer, size, MAP_MEM_VM_COPY | rw, &entry);
	if (kr == KERN_SUCCESS && (kr = map_entry(entry, size, rw, &copy)) == KERN_SUCCESS)
	{
		strcpy((char*) buffer, "written after the copy");
		check(strcmp((char*) copy, "written after the entries are gone") == 0, "a copy entry is a snapshot",
			(char*) copy);
	}
	else
		check(false, "a copy entry is a snapshot", mach_error_string(kr));

	// MAP_MEM_NAMED_CREATE: new zeroed memory that each mapping shares
	kr = make_entry(0, size, MAP_MEM_NAMED_CREATE | rw, &entry);
	if (kr == KERN_SUCCESS && (kr = map_entry(entry, size, rw, &view)) == KERN_SUCCESS &&
		(kr = map_entry(entry, size, rw, &other)) == KERN_SUCCESS)
	{
		check(((char*) view)[0] == 0 && ((char*) view)[size - 1] == 0, "new memory is zeroed", "not zero");
		strcpy((char*) view + vm_page_size, "shared");
		check(strcmp((char*) other + vm_page_size, "shared") == 0, "new memory is shared between its mappings",
			(char*) other + vm_page_size);
	}
	else
		check(false, "new memory is shared between its mappings", mach_error_string(kr));

	// memory mapped where an entry's mapping was: Darling unmaps without darlingserver knowing, and the kernel reuses the
	// address at once; sharing the new memory must share it, not what was there before
	kr = make_entry(0, size, MAP_MEM_NAMED_CREATE | rw, &entry);
	if (kr == KERN_SUCCESS && (kr = map_entry(entry, size, rw, &view)) == KERN_SUCCESS)
	{
		mach_vm_deallocate(mach_task_self(), view, size);
		mach_port_deallocate(mach_task_self(), entry);
		kr = make_entry(0, size, MAP_MEM_NAMED_CREATE | rw, &entry);
		if (kr == KERN_SUCCESS)
			kr = mach_vm_map(mach_task_self(), &view, size, 0, VM_FLAGS_FIXED, entry, 0, FALSE, rw, rw, VM_INHERIT_NONE);
		if (kr == KERN_SUCCESS)
			kr = map_entry(entry, size, rw, &other);
		if (kr == KERN_SUCCESS && (kr = make_entry(view, size, MAP_MEM_VM_SHARE | rw, &again)) == KERN_SUCCESS &&
			(kr = map_entry(again, size, rw, &copy)) == KERN_SUCCESS)
		{
			strcpy((char*) copy, "written to the new memory");
			check(strcmp((char*) other, "written to the new memory") == 0, "memory mapped at a reused address is shared",
				(char*) other);
		}
	}
	if (kr != KERN_SUCCESS)
		check(false, "memory mapped at a reused address is shared", mach_error_string(kr));

	// a range whose end rounds up past the end of the address space (it used to pass the protection check as empty)
	kr = make_entry((mach_vm_address_t) -vm_page_size, vm_page_size / 2, MAP_MEM_VM_SHARE | VM_PROT_READ, &entry);
	check(kr != KERN_SUCCESS, "a range that wraps around can't be shared", "it was");

	// an entry for at most the range's protection
	buffer = allocate(size);
	mach_vm_protect(mach_task_self(), buffer, size, FALSE, VM_PROT_READ);
	kr = make_entry(buffer, size, MAP_MEM_VM_SHARE | rw, &entry);
	check(kr == KERN_PROTECTION_FAILURE, "a read-only range can't be shared writable", mach_error_string(kr));

	return failures;
}
