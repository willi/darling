/*
 * SPDX-License-Identifier: MIT
 *
 * The interface between mldr and an outside launcher library (TAFFY_LAUNCH). It is MIT-licensed, unlike the rest of
 * mldr, so that the launcher can implement the other side of it.
 *
 * When TAFFY_LAUNCH names a library, mldr does its own set-up and nothing more:
 * - it maps the commpage and its own stack below it, and reserves the simulator's shared cache ranges, re-executing
 *   for another layout as it always does (all before the library is loaded, so the library can't land in them);
 * - it creates the darlingserver socket and the lifetime pipe, checks in, and asks for the vchroot;
 * - it strips its own argv[0] and __mldr_* variables.
 *
 * mldr then dlopen()s the library and calls TAFFY_LAUNCH_DARLING_ENTRY with a struct taffy_launch_darling. It doesn't map
 * the executable and doesn't load Darling's dyld: the launcher maps the executable and dyld, builds the initial stack
 * and apple[], and jumps. On the way, the launcher calls activate() exactly once, after which nothing may fail.
 *
 * Scope. TAFFY_LAUNCH must be an absolute Linux path: every process inherits it with its own working directory, so a
 * relative one fails every exec from another directory. It reaches a child only through the environment: mldr leaves
 * it in envp, and every Mach-O exec whose environment carries it goes to the launcher, with no fallback (Darling's own
 * programs too, if darlingserver's environment has it). An exec whose caller builds its own environment without it
 * (an explicit envp to posix_spawn() or execve(), `env -i`, a launchd job's EnvironmentVariables) is loaded by mldr
 * itself, as without this hook.
 *
 * Versions. The two sides are built and pinned together, so both structs are lockstep: a reader accepts exactly the
 * version and size it was built with and refuses anything else (mldr fails activate(); the launcher returns ENOEXEC).
 * Any change to a struct changes its version. The _Static_asserts below are a deliberate golden of the x86_64 layout:
 * the launcher's mirror of these structs asserts the same sizes and offsets.
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

// What a range in taffy_launch_darling.mapped holds.
enum {
	// The commpage, mapped read-write and filled in by mldr. The launcher owns it from the call on: it may rewrite
	// all of it and make it read-only; mldr never touches it again.
	TAFFY_LAUNCH_RANGE_COMMPAGE = 1,
	// The stack mldr maps for the program, ending at the commpage (MAP_GROWSDOWN, so it may grow below `start`).
	// mldr's own loader builds the initial stack in it; the launcher may do the same or map over it. The launcher
	// itself is called on the Linux process stack, elsewhere.
	TAFFY_LAUNCH_RANGE_MLDR_STACK = 2,
	// A PROT_NONE reservation for the simulator's dyld shared cache, which dyld_sim maps over with MAP_FIXED.
	TAFFY_LAUNCH_RANGE_SHARED_CACHE = 3,
};

// [start, end), page-aligned.
struct taffy_launch_range {
	uint64_t start;
	uint64_t end;
	uint32_t kind;     // TAFFY_LAUNCH_RANGE_*
	uint32_t reserved; // 0
};

#define TAFFY_LAUNCH_DARLING_VERSION 1

// The struct, every pointer in it, and everything they point to stay valid for the life of the process.
struct taffy_launch_darling {
	uint32_t version; // TAFFY_LAUNCH_DARLING_VERSION
	uint32_t size;    // sizeof(struct taffy_launch_darling)

	// The process as XNU's exec would see it, with mldr's own argument and variables removed. argv[0] is the
	// program's own argv[0] (for an interpreter started through binfmt_misc, its path back inside the vchroot).
	// Both arrays are NULL-terminated.
	uint64_t argc;
	char** argv;
	uint64_t envc;
	char** envp;

	// The file to execute, as a Linux path, which mldr has not opened (and which activate() gives darlingserver as
	// the process's executable path, as mldr does without the hook); and the same file as the program's own (Darwin)
	// path, for apple[]'s "executable_path=", made by the rule mldr uses for its own stack.
	const char* executable_path;
	const char* darwin_executable_path;

	// The Darwin root as a Linux path: the vchroot, or for darlingserver's first process (launchd, started with
	// __mldr_DYLD_ROOT_PATH) the path that variable names; NULL with neither. root_fd is the same directory as an
	// O_PATH descriptor, or -1. mldr places it at the top of the descriptor table, where its own sockets are, and it
	// stays open (close-on-exec) for the life of the process.
	const char* root_path;
	uint64_t root_path_length;
	int32_t root_fd;

	// mldr's darlingserver socket (also close-on-exec, at the top of the table) and Darling's elf_calls table, for
	// reference: Darling's libsystem_kernel reaches both through apple_extra.
	int32_t kernfd;
	const void* elf_calls;
	// apple[] strings that Darling's libsystem_kernel reads ("kernfd=...", "elf_calls=..."), NULL-terminated. The
	// launcher puts them in apple[] as they are, after its own keys.
	const char* const* apple_extra;

	// What mldr has mapped or reserved, in address order. The launcher maps over these with MAP_FIXED as it needs
	// to, and leaves them out of its check for mappings in its own fixed ranges.
	const struct taffy_launch_range* mapped;
	uint32_t mapped_count;
	uint32_t reserved0; // 0

	// posix_spawnattr_setbinpref_np()'s preferred CPU types, 0 for none.
	uint32_t bprefs[4];

	// Tells darlingserver about the image, then lets posix_spawn() return in the parent. Call it once, on the thread
	// mldr called the launcher on (its RPCs use that thread's socket), after everything that can fail and before the
	// jump. Before it, the launcher creates no threads, changes no signal dispositions and never dup2()s onto a
	// descriptor it wasn't given. It returns 0, or -1 with a message on stderr, after which the launcher returns an
	// error.
	int (*activate)(const struct taffy_launch_activation* activation);
};

// Never returns on success. On failure it returns the XNU errno for the parent's posix_spawn() (ENOEXEC 8,
// EBADARCH 86, EBADMACHO 88, ENOMEM 12, and so on), after writing the reason to stderr; mldr then exits. A launcher
// that returns after a successful activate() has already let the parent go on: mldr aborts.
typedef int (*taffy_launch_darling_fn)(const struct taffy_launch_darling* launch);

#if defined(__x86_64__)
#	ifdef __cplusplus
#		define TAFFY_LAUNCH_ABI(e) static_assert(e, #e)
#	else
#		define TAFFY_LAUNCH_ABI(e) _Static_assert(e, #e)
#	endif
// Golden ABI pin (x86_64): changing any of these is an incompatible change and needs a new version.
TAFFY_LAUNCH_ABI(sizeof(struct taffy_launch_activation) == 56);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_activation, version) == 0);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_activation, size) == 4);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_activation, main_stack_top) == 8);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_activation, mach_header) == 16);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_activation, dyld_all_image_infos) == 24);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_activation, dyld_all_image_infos_size) == 32);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_activation, uuid) == 40);

TAFFY_LAUNCH_ABI(sizeof(struct taffy_launch_range) == 24);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_range, start) == 0);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_range, end) == 8);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_range, kind) == 16);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_range, reserved) == 20);

TAFFY_LAUNCH_ABI(sizeof(struct taffy_launch_darling) == 136);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, version) == 0);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, size) == 4);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, argc) == 8);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, argv) == 16);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, envc) == 24);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, envp) == 32);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, executable_path) == 40);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, darwin_executable_path) == 48);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, root_path) == 56);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, root_path_length) == 64);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, root_fd) == 72);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, kernfd) == 76);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, elf_calls) == 80);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, apple_extra) == 88);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, mapped) == 96);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, mapped_count) == 104);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, reserved0) == 108);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, bprefs) == 112);
TAFFY_LAUNCH_ABI(offsetof(struct taffy_launch_darling, activate) == 128);
#	undef TAFFY_LAUNCH_ABI
#endif

#ifdef __cplusplus
}
#endif

#endif // _MLDR_TAFFY_LAUNCH_H_
