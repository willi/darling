/*
 * simiokitd: the IOKit an iOS Simulator device sees, as a booted device's host provides it.
 *
 * Usage: SIM_RUNTIME_ROOT=<RuntimeRoot> simiokitd
 *
 * On a Mac, a simulator process's IOKit talks to the Mac's kernel; with IOS_SIMULATOR_IOKITSIMD=1 (simboot.sh sets it),
 * it looks up com.apple.iokitsimd in the device instead. Darling has no kernel IOKit, so this serves that name: like
 * CoreSimulator's host services, it registers the endpoint in the device through the runtime's liblaunch_sim.dylib.
 *
 * The registry holds the root entry and IOPMrootDomain, which IORegisterForSystemPower() opens, and nothing else: there
 * is no hardware to match. Every other routine answers kIOReturnUnsupported (simiokitd_unsupported.c).
 */

#include <dlfcn.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <IOKit/IOReturn.h>

#include "iokitServer.h"

#define DEFAULT_UDID "4C0A1A5E-0000-4000-8000-000000000001"
#define SERVICE_NAME "com.apple.iokitsimd"
// how long to wait for the device's launchd_sim to come up and take the endpoint
#define REGISTER_TIMEOUT_SECONDS 60

typedef int (*launch_sim_register_endpoint_f)(const char* launchd_name, const char* service_name, mach_port_t port);

enum object_kind { ENTRY, CONNECTION, NOTIFICATION };

struct object
{
	mach_port_t port;
	enum object_kind kind;
	// the class chain from the object's own class up to OSObject
	const char* const* classes;
	const char* name;
	uint64_t id;
};

static const char* const root_classes[] = { "IORegistryEntry", "OSObject", NULL };
static const char* const pm_root_domain_classes[] = { "IOPMrootDomain", "IOService", "IORegistryEntry", "OSObject", NULL };
static const char* const user_client_classes[] = { "RootDomainUserClient", "IOUserClient", "IOService", "IORegistryEntry",
	"OSObject", NULL };
static const char* const notification_classes[] = { "IOServiceMessageUserNotification", "IOUserNotification",
	"OSIterator", "OSObject", NULL };

static mach_port_t port_set;
static struct object* objects;
static size_t object_count;
static uint64_t next_id = 0x100000100;
// the registry's entries; objects move around in `objects`, so they are kept by port
static mach_port_t root;
static mach_port_t pm_root_domain;

// send rights handed out in the current reply, which simiokitd drops once the reply is sent: from then on, the
// clients' rights are the only ones, and the no-senders notification tells when they are all gone
static mach_port_t pending_release[2];
static size_t pending_release_count;

static struct object* object_for_port(mach_port_t port)
{
	size_t i;

	for (i = 0; i < object_count; i++)
	{
		if (objects[i].port == port)
			return &objects[i];
	}
	return NULL;
}

// A new object with a receive right in the port set and one send right; registry entries keep theirs for good.
static struct object* object_create(enum object_kind kind, const char* const* classes, const char* name)
{
	struct object* grown;
	mach_port_t port, previous;
	kern_return_t kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
	if (kr != KERN_SUCCESS)
		return NULL;
	if (mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS
		|| mach_port_move_member(mach_task_self(), port, port_set) != KERN_SUCCESS)
		goto fail;
	if (kind != ENTRY)
	{
		// once its make-send count is 1, i.e. once the right made above is the only one ever made
		kr = mach_port_request_notification(mach_task_self(), port, MACH_NOTIFY_NO_SENDERS, 1, port,
			MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);
		if (kr != KERN_SUCCESS)
			goto fail;
	}

	grown = realloc(objects, (object_count + 1) * sizeof(*objects));
	if (grown == NULL)
		goto fail;
	objects = grown;
	objects[object_count] = (struct object) { .port = port, .kind = kind, .classes = classes, .name = name,
		.id = next_id++ };
	return &objects[object_count++];

fail:
	mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
	mach_port_deallocate(mach_task_self(), port);
	return NULL;
}

// Hands a new connection or notification out in the reply being built: simiokitd drops its own send right after the
// reply. Registry entries keep theirs, so they are handed out as their ports.
static mach_port_t object_hand_out(struct object* object)
{
	pending_release[pending_release_count++] = object->port;
	return object->port;
}

static void object_destroy(struct object* object)
{
	mach_port_mod_refs(mach_task_self(), object->port, MACH_PORT_RIGHT_RECEIVE, -1);
	*object = objects[--object_count];
}

static bool object_conforms_to(const struct object* object, const char* class_name)
{
	const char* const* c;

	for (c = object->classes; *c != NULL; c++)
	{
		if (strcmp(*c, class_name) == 0)
			return true;
	}
	return false;
}

kern_return_t is_io_object_get_class(mach_port_t object_port, io_name_t className)
{
	struct object* object = object_for_port(object_port);

	if (object == NULL)
		return kIOReturnBadArgument;
	strlcpy(className, object->classes[0], sizeof(io_name_t));
	return kIOReturnSuccess;
}

kern_return_t is_io_object_conforms_to(mach_port_t object_port, io_name_t className, boolean_t* conforms)
{
	struct object* object = object_for_port(object_port);

	if (object == NULL)
		return kIOReturnBadArgument;
	*conforms = object_conforms_to(object, className);
	return kIOReturnSuccess;
}

kern_return_t is_io_registry_entry_get_name(mach_port_t entry_port, io_name_t name)
{
	struct object* object = object_for_port(entry_port);

	if (object == NULL || object->kind == NOTIFICATION)
		return kIOReturnBadArgument;
	strlcpy(name, object->name, sizeof(io_name_t));
	return kIOReturnSuccess;
}

kern_return_t is_io_registry_entry_get_registry_entry_id(mach_port_t entry_port, uint64_t* entry_id)
{
	struct object* object = object_for_port(entry_port);

	if (object == NULL || object->kind == NOTIFICATION)
		return kIOReturnBadArgument;
	*entry_id = object->id;
	return kIOReturnSuccess;
}

kern_return_t is_io_registry_get_root_entry(mach_port_t master_port, mach_port_t* entry)
{
	*entry = root;
	return kIOReturnSuccess;
}

// "<plane>:/<names>" or "<plane>:<names>": the planes' roots, and IOPMrootDomain wherever its plane puts it
kern_return_t is_io_registry_entry_from_path(mach_port_t master_port, io_string_t path, mach_port_t* entry)
{
	const char* names = strchr(path, ':');
	const char* last;

	if (names == NULL)
		return kIOReturnNotFound;
	names++;
	while (*names == '/')
		names++;
	last = strrchr(names, '/');
	last = last ? last + 1 : names;

	if (*names == '\0')
		*entry = root;
	else if (strcmp(last, "IOPMrootDomain") == 0)
		*entry = pm_root_domain;
	else
		return kIOReturnNotFound;
	return kIOReturnSuccess;
}

kern_return_t is_io_service_open_extended(mach_port_t service, task_t owningTask, uint32_t connect_type, NDR_record_t ndr,
	io_buf_ptr_t properties, mach_msg_type_number_t propertiesCnt, kern_return_t* result, mach_port_t* connection)
{
	struct object* object = object_for_port(service);
	struct object* client;

	if (object == NULL || object->kind != ENTRY)
		return kIOReturnBadArgument;

	// on success, the request's rights and memory are the server's
	mach_port_deallocate(mach_task_self(), owningTask);
	if (propertiesCnt > 0)
		vm_deallocate(mach_task_self(), (vm_address_t) properties, propertiesCnt);

	*connection = MACH_PORT_NULL;
	if (service != pm_root_domain)
	{
		*result = kIOReturnUnsupported;
		return kIOReturnSuccess;
	}

	client = object_create(CONNECTION, user_client_classes, "RootDomainUserClient");
	if (client == NULL)
	{
		*result = kIOReturnNoMemory;
		return kIOReturnSuccess;
	}
	*connection = object_hand_out(client);
	*result = kIOReturnSuccess;
	return kIOReturnSuccess;
}

kern_return_t is_io_service_close(mach_port_t connection)
{
	struct object* object = object_for_port(connection);

	if (object == NULL || object->kind != CONNECTION)
		return kIOReturnBadArgument;
	object_destroy(object);
	return kIOReturnSuccess;
}

// Interest notifications are accepted, but the device never sleeps or wakes, so none is ever sent.
static kern_return_t add_interest_notification(mach_port_t service, mach_port_t wake_port, mach_port_t* notification)
{
	struct object* object = object_for_port(service);
	struct object* note;

	if (object == NULL || object->kind != ENTRY)
		return kIOReturnBadArgument;
	note = object_create(NOTIFICATION, notification_classes, "IOServiceMessageUserNotification");
	if (note == NULL)
		return kIOReturnNoMemory;

	mach_port_deallocate(mach_task_self(), wake_port);
	*notification = object_hand_out(note);
	return kIOReturnSuccess;
}

kern_return_t is_io_service_add_interest_notification(mach_port_t service, io_name_t type_of_interest,
	mach_port_t wake_port, io_async_ref_t reference, mach_msg_type_number_t referenceCnt, mach_port_t* notification)
{
	return add_interest_notification(service, wake_port, notification);
}

kern_return_t is_io_service_add_interest_notification_64(mach_port_t service, io_name_t type_of_interest,
	mach_port_t wake_port, io_async_ref64_t reference, mach_msg_type_number_t referenceCnt, mach_port_t* notification)
{
	return add_interest_notification(service, wake_port, notification);
}

static int register_endpoint(mach_port_t port)
{
	const char* root_path = getenv("SIM_RUNTIME_ROOT");
	const char* udid = getenv("SIM_UDID");
	launch_sim_register_endpoint_f register_endpoint;
	char lib[PATH_MAX], name[128];
	void* handle;
	int i, err;

	if (root_path == NULL)
	{
		fprintf(stderr, "usage: SIM_RUNTIME_ROOT=<RuntimeRoot> simiokitd\n");
		return 64;
	}
	snprintf(name, sizeof(name), "com.apple.CoreSimulator.SimDevice.%s", udid ? udid : DEFAULT_UDID);
	snprintf(lib, sizeof(lib), "%s/usr/lib/system/host/liblaunch_sim.dylib", root_path);

	handle = dlopen(lib, RTLD_NOW);
	if (handle == NULL)
	{
		fprintf(stderr, "simiokitd: %s\n", dlerror());
		return 69;
	}
	register_endpoint = (launch_sim_register_endpoint_f) dlsym(handle, "launch_sim_register_endpoint");
	if (register_endpoint == NULL)
	{
		fprintf(stderr, "simiokitd: %s lacks launch_sim_register_endpoint\n", lib);
		return 69;
	}

	// simboot.sh starts this along with the device, whose launchd_sim checks in a little later
	for (i = 0; i < REGISTER_TIMEOUT_SECONDS * 10; i++)
	{
		err = register_endpoint(name, SERVICE_NAME, port);
		if (err == 0)
			return 0;
		usleep(100 * 1000);
	}
	fprintf(stderr, "simiokitd: launch_sim_register_endpoint(%s, %s): %d (%s); is the device booted?\n", name,
		SERVICE_NAME, err, mach_error_string(err));
	return 69;
}

int main(int argc, char** argv)
{
	union { union __RequestUnion__is_iokit_subsystem request; mach_msg_header_t head;
		uint8_t buffer[sizeof(union __RequestUnion__is_iokit_subsystem) + MAX_TRAILER_SIZE]; } in;
	union { union __ReplyUnion__is_iokit_subsystem reply; mig_reply_error_t error; } out;
	mach_port_t service;
	struct object* object;
	kern_return_t kr;
	bool debug;
	size_t i;
	int err;

	// SIMIOKITD_DEBUG=1: each request and how it was answered, on stderr
	debug = getenv("SIMIOKITD_DEBUG") != NULL;

	if (argc > 1)
	{
		fprintf(stderr, "usage: SIM_RUNTIME_ROOT=<RuntimeRoot> %s\n", argv[0]);
		return 64;
	}

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &port_set) != KERN_SUCCESS
		|| mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &service) != KERN_SUCCESS
		|| mach_port_insert_right(mach_task_self(), service, service, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS
		|| mach_port_move_member(mach_task_self(), service, port_set) != KERN_SUCCESS)
	{
		fprintf(stderr, "simiokitd: can't set up the service port\n");
		return 71;
	}
	object = object_create(ENTRY, root_classes, "Root");
	root = object ? object->port : MACH_PORT_NULL;
	object = object ? object_create(ENTRY, pm_root_domain_classes, "IOPMrootDomain") : NULL;
	if (object == NULL)
	{
		fprintf(stderr, "simiokitd: can't create the registry\n");
		return 71;
	}
	pm_root_domain = object->port;

	err = register_endpoint(service);
	if (err != 0)
		return err;

	for (;;)
	{
		kr = mach_msg(&in.head, MACH_RCV_MSG, 0, sizeof(in), port_set, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (kr != MACH_MSG_SUCCESS)
		{
			fprintf(stderr, "simiokitd: mach_msg receive: %d (%s)\n", kr, mach_error_string(kr));
			continue;
		}

		// the last client released a connection or a notification
		if (in.head.msgh_id == MACH_NOTIFY_NO_SENDERS)
		{
			object = object_for_port(in.head.msgh_local_port);
			if (object != NULL && object->kind != ENTRY)
				object_destroy(object);
			mach_msg_destroy(&in.head);
			continue;
		}

		pending_release_count = 0;
		iokit_server(&in.head, &out.error.Head);
		if (debug)
		{
			fprintf(stderr, "simiokitd: routine %d on 0x%x: %s 0x%x\n", in.head.msgh_id, in.head.msgh_local_port,
				(out.error.Head.msgh_bits & MACH_MSGH_BITS_COMPLEX) ? "complex reply" : "RetCode", out.error.RetCode);
			fflush(stderr);
		}

		// as mach_msg_server() does: a failed request is the server's to destroy, and some get no reply at all
		if (!(out.error.Head.msgh_bits & MACH_MSGH_BITS_COMPLEX) && out.error.RetCode != KERN_SUCCESS)
		{
			if (out.error.RetCode == MIG_NO_REPLY)
				out.error.Head.msgh_remote_port = MACH_PORT_NULL;
			else
			{
				in.head.msgh_remote_port = MACH_PORT_NULL;
				mach_msg_destroy(&in.head);
			}
		}
		if (out.error.Head.msgh_remote_port != MACH_PORT_NULL)
		{
			kr = mach_msg(&out.error.Head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, out.error.Head.msgh_size, 0,
				MACH_PORT_NULL, 0, MACH_PORT_NULL);
			if (kr != MACH_MSG_SUCCESS)
				mach_msg_destroy(&out.error.Head);
		}

		for (i = 0; i < pending_release_count; i++)
			mach_port_deallocate(mach_task_self(), pending_release[i]);
	}
}
