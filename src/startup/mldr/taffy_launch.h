/*
 * SPDX-License-Identifier: MIT
 *
 * The interface between mldr and an outside launcher library (TAFFY_LAUNCH). It is MIT-licensed, unlike the rest of
 * mldr, so that the launcher can implement the other side of it.
 *
 * When TAFFY_LAUNCH names a library, mldr does its own set-up and nothing more:
 * - it reserves the commpage, its stack below it and the simulator's shared cache ranges, re-executing for another
 *   layout as it always does;
 * - it creates the darlingserver socket and the lifetime pipe, checks in, and asks for the vchroot;
 * - it strips its own argv[0] and __mldr_* variables.
 *
 * mldr then dlopen()s the library and calls TAFFY_LAUNCH_DARLING_ENTRY with a struct taffy_launch_darling. It doesn't map
 * the executable and doesn't load Darling's dyld: the launcher maps the executable and dyld, builds the initial stack
 * and apple[], and jumps. On the way, the launcher calls activate() exactly once, after which nothing may fail.
 *
 * Both structs start with a version and their size. A newer mldr only appends fields, so a launcher reads the fields it
 * knows when size covers them.
 */

#ifndef _MLDR_TAFFY_LAUNCH_H_
#define _MLDR_TAFFY_LAUNCH_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TAFFY_LAUNCH_ENV "TAFFY_LAUNCH"
#define TAFFY_LAUNCH_DARLING_ENTRY "taffy_launch_darling"

#define TAFFY_LAUNCH_ACTIVATION_VERSION 1

// What only the launcher knows once it has mapped the image. mldr tells darlingserver, then returns.
struct taffy_launch_activation {
	uint32_t version; // TAFFY_LAUNCH_ACTIVATION_VERSION
	uint32_t size;    // sizeof(struct taffy_launch_activation)

	// The main thread's stack top, as kern.usrstack64 reports it (Darling's libsystem_kernel asks
	// elf_calls->darling_thread_get_stack for it).
	uint64_t main_stack_top;
	// The address of the main executable's mach_header.
	uint64_t mach_header;
	// The dyld_all_image_infos that TASK_DYLD_INFO reports for this task (darlingserver's set_dyld_info).
	uint64_t dyld_all_image_infos;
	uint64_t dyld_all_image_infos_size;
	// The main executable's LC_UUID, or zeros (darlingserver's set_executable_path).
	uint8_t uuid[16];
};

#define TAFFY_LAUNCH_DARLING_VERSION 1

struct taffy_launch_darling {
	uint32_t version; // TAFFY_LAUNCH_DARLING_VERSION
	uint32_t size;    // sizeof(struct taffy_launch_darling)

	// The process as XNU's exec would see it, with mldr's own argument and variables removed. argv[0] is the
	// program's own argv[0] (for an interpreter started through binfmt_misc, its path back inside the vchroot).
	// Both arrays are NULL-terminated and stay valid for the life of the process.
	uint64_t argc;
	char** argv;
	uint64_t envc;
	char** envp;

	// The file to execute, as a Linux path, which mldr has not opened.
	const char* executable_path;

	// The vchroot: the Linux path of the Darwin root, or NULL with no vchroot, and the same directory as an O_PATH
	// descriptor (close-on-exec), or -1. A Darwin path outside it gets system_root ahead of it (apple[]'s
	// executable_path, as mldr wrote it).
	const char* root_path;
	uint64_t root_path_length;
	int32_t root_fd;
	const char* system_root;

	// Passed through to Darling's libsystem_kernel in apple[] as "kernfd=%d" and "elf_calls=%lx" (hex, no 0x).
	int32_t kernfd;
	const void* elf_calls;

	// What mldr has already mapped: the commpage (read-write, filled in by mldr) and its own stack, which ends at
	// mldr_stack_top (the commpage's address).
	uint64_t commpage_address;
	uint64_t mldr_stack_top;

	// posix_spawnattr_setbinpref_np()'s preferred CPU types, 0 for none.
	uint32_t bprefs[4];

	// Tells darlingserver about the image, then lets posix_spawn() return in the parent. Call it once, after
	// everything that can fail and before the jump. It returns 0, or -1 with a message on stderr, after which the
	// launcher returns an error.
	int (*activate)(const struct taffy_launch_activation* activation);
};

// Never returns on success. On failure it returns the XNU errno for the parent's posix_spawn() (ENOEXEC 8,
// EBADARCH 86, EBADMACHO 88, ENOMEM 12, and so on), after writing the reason to stderr. mldr then exits.
typedef int (*taffy_launch_darling_fn)(const struct taffy_launch_darling* launch);

#ifdef __cplusplus
}
#endif

#endif // _MLDR_TAFFY_LAUNCH_H_
