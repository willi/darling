#ifndef _ELFCALLS_H_
#define _ELFCALLS_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// What mldr calls back into libsystem_kernel for on a Darwin thread it starts. libsystem_kernel has one instance.
struct darling_thread_create_callbacks {
	// sizeof(struct darling_thread_create_callbacks) as the instance's definer saw it: mldr calls only the hooks it
	// covers, so a hook added here works with a libsystem_kernel that doesn't set it yet
	size_t size;
	unsigned int (*thread_self_trap)(void);
	void (*thread_set_tsd_base)(void*, int);
	void (*rpc_guard)(int);
	void (*rpc_unguard)(int);
	// Gives the new thread what Darling's signal handlers need before mldr unblocks its signals: its alternate signal
	// stack. Answers 0, or a negative errno, and then mldr doesn't start the thread. What it stores in *stack goes to
	// signal_stack_teardown when the thread ends.
	int (*signal_stack_setup)(void** stack);
	// Frees what signal_stack_setup set up. The thread is on its way out, with every signal blocked.
	void (*signal_stack_teardown)(void* stack);
};

// The hook `field` of `callbacks`, or NULL if its definer's struct is too short to have it
#define DARLING_THREAD_CREATE_CALLBACK(callbacks, field) \
	(((callbacks)->size >= offsetof(struct darling_thread_create_callbacks, field) + sizeof((callbacks)->field)) \
		? (callbacks)->field : NULL)

typedef const struct darling_thread_create_callbacks* darling_thread_create_callbacks_t;

struct elf_calls
{
	// ELF dynamic loader access
	void* (*dlopen)(const char* name);
	int (*dlclose)(void* lib);
	void* (*dlsym)(void* lib, const char* name);
	char* (*dlerror)(void);

	// pthread wrapping
	void* (*darling_thread_create)(unsigned long stack_size, unsigned long pthobj_size,
                void* entry_point, uintptr_t arg3,
                uintptr_t arg4, uintptr_t arg5, uintptr_t arg6,
                darling_thread_create_callbacks_t callbacks, void* dthread);
	int (*darling_thread_terminate)(void* stackaddr,
				unsigned long freesize, unsigned long pthobj_size);

	// this returns the address of the main thread's stack
	void* (*darling_thread_get_stack)(void);

	// The same as above, except they abort() in case of failure
	void* (*dlopen_fatal)(const char* name);
	int (*dlclose_fatal)(void* lib);
	void* (*dlsym_fatal)(void* lib, const char* name);

	// POSIX semaphore APIs
	int (*get_errno)(void);
	int* (*sem_open)(const char* name, int oflag, unsigned short mode, unsigned int value);
	int (*sem_wait)(int* sem);
	int (*sem_trywait)(int* sem);
	int (*sem_post)(int* sem);
	int (*sem_close)(int* sem);
	int (*sem_unlink)(const char* name);

	// POSIX SHM APIs
	int (*shm_open)(const char* name, int oflag, unsigned short mode);
	int (*shm_unlink)(const char* name);

	void (*exit)(int ec);

	// Memory allocation
	void* (*malloc)(size_t size);
	void (*free)(void *ptr);
	void* (*realloc)(void *ptr, size_t size);

	// POSIX sysconf
	long (*sysconf)(int name);

	// darlingserver RPC info
	const void* (*dserver_socket_address)(void);
	// aborts on a thread that has none (see dserver_thread_has_socket)
	int (*dserver_per_thread_socket)(void);
	void (*dserver_per_thread_socket_refresh)(void);
	void (*dserver_close_socket)(int socket);

	// darlingserver process lifetime pipe info
	int (*dserver_get_process_lifetime_pipe)(void);
	int (*dserver_process_lifetime_pipe_refresh)(void);
	void (*dserver_close_process_lifetime_pipe)(int fd);

	// Around a fork, which is made with the raw syscall, so glibc's fork handlers don't run: mldr's locks are taken
	// before it and released after it in both processes, so the child doesn't inherit one another thread held.
	void (*fork_prepare)(void);
	void (*fork_parent)(void);
	void (*fork_child)(void);

	// Whether this thread can make darlingserver calls: the main thread, or a thread mldr started, which has checked
	// in. A thread that a host library started with glibc's pthread_create never can. Safe in a signal handler.
	bool (*dserver_thread_has_socket)(void);
};

#endif

