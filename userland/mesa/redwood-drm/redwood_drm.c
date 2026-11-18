/*
 * The libdrm of xf86drm.h (see there): Linux's DRM requests to the kernel's GPU driver as calls
 * of its IOKit user client. A request's number (the low byte of the ioctl code) is the call's
 * selector, its structure goes in and comes back as it is; a buffer's memory is the client
 * memory of the type that is the buffer's handle.
 *
 * freedreno knows its device by a file descriptor. It gets one that means nothing (/dev/null):
 * there is one GPU and one connection to it in a process.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <stdio.h>
#include <mach/mach_time.h>
#include <IOKit/IOKitLib.h>

#include "xf86drm.h"

#define REDWOOD_GPU_SERVICE     "SweetGPU"      /* iokit/Kernel/arm/SweetGPU.cpp (op5) */
#define REDWOOD_GPU_SURFACE     0x7e            /* the request that is not Linux's */
#define REDWOOD_GPU_DESCRIBE    0xffffffffu     /* ... asked only what the buffer would be like */

static io_connect_t connection;
static int connection_fd = -1;

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	io_service_t service;

	(void)busid;
	(void)type;
	if (strcmp(name, "msm") != 0) {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	service = IOServiceGetMatchingService(0, IOServiceMatching(REDWOOD_GPU_SERVICE));
	if (service == 0) {
		return -1;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
		IOObjectRelease(service);
		return -1;
	}
	IOObjectRelease(service);
	connection_fd = open("/dev/null", O_RDWR);
	return dup(connection_fd);
}

/*
 * (op5) What the kernel is asked, and how long it takes: a line on stderr every two seconds
 * ("redwood: ..."), per request number the calls and microseconds, and the buffer maps and unmaps.
 * Environment REDWOOD_STATS=0 turns it off.
 */
#define STAT_SLOTS 0x100
static uint64_t stat_calls[STAT_SLOTS], stat_ticks[STAT_SLOTS];
static uint64_t stat_maps, stat_map_ticks, stat_unmaps, stat_unmap_ticks, stat_window;
static int stat_on = -1;

static uint64_t
ticks_to_us(uint64_t ticks)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return ticks * base.numer / base.denom / 1000;
}

static void
stat_report(uint64_t now)
{
	char line[1024];
	int n;

	if (stat_on < 0) {
		const char *setting = getenv("REDWOOD_STATS");

		stat_on = setting == NULL || strcmp(setting, "0") != 0;
	}
	if (!stat_on) {
		return;
	}
	if (stat_window == 0) {
		stat_window = now;
		return;
	}
	if (ticks_to_us(now - stat_window) < 2000000) {
		return;
	}
	n = snprintf(line, sizeof(line), "redwood: %llu ms: map %llu/%llu us unmap %llu/%llu us",
	    ticks_to_us(now - stat_window) / 1000, stat_maps, ticks_to_us(stat_map_ticks), stat_unmaps,
	    ticks_to_us(stat_unmap_ticks));
	for (unsigned i = 0; i < STAT_SLOTS && n < (int)sizeof(line) - 40; i++) {
		if (stat_calls[i] != 0) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, "; %#x %llu/%llu us", i, stat_calls[i],
			    ticks_to_us(stat_ticks[i]));
		}
	}
	if (n < (int)sizeof(line) - 1) {
		line[n++] = '\n';
	}
	write(2, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line)));
	memset(stat_calls, 0, sizeof(stat_calls));
	memset(stat_ticks, 0, sizeof(stat_ticks));
	stat_maps = stat_map_ticks = stat_unmaps = stat_unmap_ticks = 0;
	stat_window = now;
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	uint64_t began = mach_absolute_time();
	kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);
	uint64_t ended = mach_absolute_time();

	stat_calls[number % STAT_SLOTS]++;
	stat_ticks[number % STAT_SLOTS] += ended - began;
	stat_report(ended);

	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	int error = call((unsigned)(request & 0xff), arg, IOCPARM_LEN(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

/*
 * (op5) DRM_MSM_GEM_MADVISE: the kernel's driver has nothing that takes memory back from idle
 * buffers and answers "retained" to every request (SweetGPU.cpp). freedreno's buffer cache asks it
 * thousands of times a second (12,000 calls, ~240 ms every 2 s in backboardd): answered here.
 */
#define REDWOOD_MSM_GEM_MADVISE 0x08
struct redwood_msm_gem_madvise { uint32_t handle, madv, retained; };

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	if (index == REDWOOD_MSM_GEM_MADVISE && size == sizeof(struct redwood_msm_gem_madvise)) {
		((struct redwood_msm_gem_madvise *)data)->retained = 1;
		return 0;
	}
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/*
 * The driver calls itself what Linux's is called, and is of the version whose interface it has:
 * 1.7, buffers at addresses the kernel chooses and tells ("softpin"), nothing newer.
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = 7;
		version->name = strdup("msm");
		version->name_len = 3;
		version->date = strdup("");
		version->desc = strdup("Adreno on Darwin");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

void *
redwood_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	uint64_t began = mach_absolute_time();
	kern_return_t result;

	(void)fd;
	(void)size;
	/*
	 * Linux gives freedreno its buffers write-combined, with no cache of this processor's between
	 * the program and the memory the GPU sees. So does the kernel's driver here: the memory is
	 * made that way, and this mapping is as the memory is.
	 */
	/*
	 * (op5) The cache mode a client asks for here replaces the kernel's
	 * (IOUserClient::mapClientMemory64), so ask for write-combine
	 * explicitly: with a cached mapping the commands stayed in this CPU's
	 * cache and the GPU read zeros (test 0131).
	 */
	result = IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped,
	    kIOMapAnywhere | kIOMapWriteCombineCache);
	stat_maps++;
	stat_map_ticks += mach_absolute_time() - began;
	if (result != 0) {
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	return (void *)(uintptr_t)address;
}

void
redwood_drm_unmap(int fd, uint32_t handle, void *address)
{
	uint64_t began = mach_absolute_time();

	(void)fd;
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
	stat_unmaps++;
	stat_unmap_ticks += mach_absolute_time() - began;
}

int
redwood_drm_surface(int fd, struct redwood_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = REDWOOD_GPU_DESCRIBE;
	error = call(REDWOOD_GPU_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

/*
 * (op5, xnu 0137) The GPU has drawn IOSurface `surface`: its seed as it is now needs no push out
 * of the processor's cache at the next submission (SweetGPU.cpp SWEET_GPU_SURFACE_DRAWN). For the
 * CA bridge (eagl.c, through dlsym: Mesa's own symbols are hidden), over Mesa's connection, whose
 * buffers they are. -1 when the kernel does not know the request (before 0137).
 */
#define REDWOOD_GPU_SURFACE_DRAWN 0x7d
__attribute__((visibility("default"))) int redwood_drm_surface_drawn(uint32_t surface);
int
redwood_drm_surface_drawn(uint32_t surface)
{
	struct {
		uint32_t surface, marked;
	} request = { .surface = surface };

	if (connection == 0 || call(REDWOOD_GPU_SURFACE_DRAWN, &request, sizeof(request)) != 0) {
		return -1;
	}
	return (int)request.marked;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct redwood_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(REDWOOD_GPU_SURFACE, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

/* Buffers and fences as file descriptors, for handing them to other programs and drivers: none of that here. */
#define NONE(name, ...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
NONE(drmSyncobjCreate, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t flags,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjDestroy, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle)
NONE(drmSyncobjReset, __attribute__((unused)) int fd, __attribute__((unused)) const uint32_t *handles,
    __attribute__((unused)) uint32_t count)
NONE(drmSyncobjFDToHandle, __attribute__((unused)) int fd, __attribute__((unused)) int obj_fd,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *obj_fd)
NONE(drmSyncobjImportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int sync_file_fd)
NONE(drmSyncobjExportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *sync_file_fd)
