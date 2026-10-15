// mach_vm_protect() of a range that starts inside a page protects the pages it touches, as on XNU. Darling's passed the
// address to Linux's mprotect(), which wants it page-aligned, and failed: ColorSync couldn't make its conversion tables
// read-only, and the render server drew no image with a color profile (app artwork, home screen icons).
//
// Prints one "ok"/"not ok" line per check; the exit status is the number of failed checks.

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>

static int failures;

static void check(int ok, const char* what)
{
	printf("%s - %s\n", ok ? "ok" : "not ok", what);
	if (!ok)
		failures++;
}

int main(void)
{
	mach_vm_address_t page = 0;
	vm_size_t ps = vm_page_size;
	mach_vm_allocate(mach_task_self(), &page, 2 * ps, VM_FLAGS_ANYWHERE);
	char* p = (char*) page;
	p[0] = 1;

	// the middle of the first page into the second
	check(mach_vm_protect(mach_task_self(), page + 0x6f0, ps, FALSE, VM_PROT_READ) == KERN_SUCCESS,
		"an unaligned range can be made read-only");

	mach_vm_address_t addr = page;
	mach_vm_size_t size = 0;
	vm_region_basic_info_data_64_t info;
	mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t obj;
	kern_return_t kr = mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t) &info,
		&count, &obj);
	check(kr == KERN_SUCCESS && addr == page && info.protection == VM_PROT_READ,
		"the page the range starts in is read-only");

	check(mach_vm_protect(mach_task_self(), page + 0x6f0, ps, FALSE, VM_PROT_READ | VM_PROT_WRITE) == KERN_SUCCESS &&
			(p[0] = 2, p[ps] = 2, p[0] == 2),
		"and writable again");

	mach_vm_deallocate(mach_task_self(), page, 2 * ps);
	return failures;
}
