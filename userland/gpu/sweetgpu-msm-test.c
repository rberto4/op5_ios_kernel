/*
 * sweetgpu-msm-test: user-space check of the SweetGPU msm interface (phase E).
 *
 * Talks to the kernel exactly as Mesa's freedreno does through
 * userland/mesa/redwood-drm: IOConnectCallStructMethod with selector
 * DRM_COMMAND_BASE + DRM_MSM_x and the msm_drm.h structure in and out,
 * buffers mapped with IOConnectMapMemory64 (type = handle).
 *
 *   1. GET_PARAM: GPU id, chip id, GMEM, max clock, VA range, timestamp
 *   2. two buffers (commands, data), GPU addresses, CPU mappings
 *   3. a PM4 command buffer: CP_MEM_WRITE of 256 dwords into the data buffer
 *   4. GEM_SUBMIT, WAIT_FENCE, CPU_PREP, check every dword
 *   5. 200 submit + wait round trips: average latency
 *   6. (kernel 0134+) an IOSurface as a buffer (request 0x7e): the GPU writes
 *      256 dwords into the surface, read back through IOSurface's own mapping
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 \
 *        -framework IOKit -framework IOSurface -framework CoreFoundation -o sweetgpu-msm-test sweetgpu-msm-test.c
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>

/* include/uapi/drm/msm_drm.h (Copyright (C) 2013 Red Hat, MIT license) */
#define DRM_COMMAND_BASE                0x40
#define DRM_GEM_CLOSE                   0x09
#define DRM_MSM_GET_PARAM               0x00
#define DRM_MSM_GEM_NEW                 0x02
#define DRM_MSM_GEM_INFO                0x03
#define DRM_MSM_GEM_CPU_PREP            0x04
#define DRM_MSM_GEM_SUBMIT              0x06
#define DRM_MSM_WAIT_FENCE              0x07
#define DRM_MSM_SUBMITQUEUE_NEW         0x0A
#define MSM_INFO_GET_IOVA               0x01
#define MSM_SUBMIT_CMD_BUF              0x0001
#define MSM_SUBMIT_BO_READ              0x0001
#define MSM_SUBMIT_BO_WRITE             0x0002
#define MSM_PREP_READ                   0x01
#define MSM_BO_WC                       0x00020000
struct drm_msm_timespec { int64_t tv_sec, tv_nsec; };
struct drm_msm_param { uint32_t pipe, param; uint64_t value; uint32_t len, pad; };
struct drm_msm_gem_new { uint64_t size; uint32_t flags, handle; };
struct drm_msm_gem_info { uint32_t handle, info; uint64_t value; uint32_t len, pad; };
struct drm_msm_gem_cpu_prep { uint32_t handle, op; struct drm_msm_timespec timeout; };
struct drm_msm_gem_submit_cmd { uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs; uint64_t relocs; };
struct drm_msm_gem_submit_bo { uint32_t flags, handle; uint64_t presumed; };
struct drm_msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct drm_msm_wait_fence { uint32_t fence, flags; struct drm_msm_timespec timeout; uint32_t queueid; };
struct drm_msm_submitqueue { uint32_t flags, prio, id; };
struct drm_gem_close { uint32_t handle, pad; };
#define SWEET_GPU_SURFACE               0x7e    /* SweetGPU.cpp (0134): an IOSurface's memory as a buffer */
struct sweet_surface_request {
	uint32_t surface, handle;
	uint64_t size;
	uint32_t offset, row_bytes, width, height, pixel_format, planes;
};

#define CP_TYPE7_PKT    0x70000000u
#define CP_MEM_WRITE    61
#define N_WORDS         256

static io_connect_t conn;

static int
call(uint32_t nr, void *s, size_t size)
{
	size_t out = size;
	kern_return_t kr = IOConnectCallStructMethod(conn, nr, s, size, s, &out);
	return kr == KERN_SUCCESS ? 0 : (int)kr;
}

static uint32_t
parity(uint32_t v)
{
	return (0x9669u >> (0xfu & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^
	       (v >> 28)))) & 1u;
}

static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return CP_TYPE7_PKT | cnt | (parity(cnt) << 15) | ((op & 0x7f) << 16) | (parity(op) << 23);
}

static int
bo_new(uint64_t size, uint32_t *handle, uint64_t *iova, volatile uint32_t **cpu)
{
	struct drm_msm_gem_new n = { .size = size, .flags = MSM_BO_WC };
	if (call(DRM_COMMAND_BASE + DRM_MSM_GEM_NEW, &n, sizeof(n)) != 0) {
		return -1;
	}
	struct drm_msm_gem_info i = { .handle = n.handle, .info = MSM_INFO_GET_IOVA };
	if (call(DRM_COMMAND_BASE + DRM_MSM_GEM_INFO, &i, sizeof(i)) != 0) {
		return -1;
	}
	mach_vm_address_t addr = 0;
	mach_vm_size_t mapped = 0;
	/* the caller's cache mode wins over the kernel's (IOUserClient::mapClientMemory64): ask for WC */
	if (IOConnectMapMemory64(conn, n.handle, mach_task_self(), &addr, &mapped,
	    kIOMapAnywhere | kIOMapWriteCombineCache) != KERN_SUCCESS) {
		return -1;
	}
	*handle = n.handle;
	*iova = i.value;
	*cpu = (volatile uint32_t *)(uintptr_t)addr;
	return 0;
}

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static CFNumberRef
num(int v)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &v);
}

/* 1: the GPU wrote into an IOSurface imported with 0x7e, 0: it did not, -1: no import */
static int
surface_test(uint32_t hc, uint64_t ic, volatile uint32_t *cc, uint32_t queue)
{
	const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat };
	const void *vals[] = { num(64), num(64), num(4), num('BGRA') };
	CFDictionaryRef props = CFDictionaryCreate(NULL, keys, vals, 4, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	IOSurfaceRef surf = IOSurfaceCreate(props);
	CFRelease(props);
	if (surf == NULL) {
		printf("surface: IOSurfaceCreate failed\n");
		return -1;
	}
	uint32_t sid = IOSurfaceGetID(surf);
	IOSurfaceLock(surf, 0, NULL);
	volatile uint32_t *px = (volatile uint32_t *)IOSurfaceGetBaseAddress(surf);
	for (int k = 0; k < N_WORDS; k++) {
		px[k] = 0x5a5a5a5a;             /* this processor writes it first: the kernel must push that out */
	}
	IOSurfaceUnlock(surf, 0, NULL);
	struct sweet_surface_request d = { .surface = sid, .handle = 0xffffffffu };
	int r = call(SWEET_GPU_SURFACE, &d, sizeof(d));
	printf("surface %u describe: %d size %llu offset %u row %u %ux%u fmt 0x%x planes %u\n", sid, r,
	    (unsigned long long)d.size, d.offset, d.row_bytes, d.width, d.height, d.pixel_format, d.planes);
	struct sweet_surface_request q = { .surface = sid };
	if (r != 0 || (r = call(SWEET_GPU_SURFACE, &q, sizeof(q))) != 0) {
		printf("surface: import failed 0x%x\n", r);
		CFRelease(surf);
		return -1;
	}
	struct drm_msm_gem_info i = { .handle = q.handle, .info = MSM_INFO_GET_IOVA };
	r = call(DRM_COMMAND_BASE + DRM_MSM_GEM_INFO, &i, sizeof(i));
	uint64_t at = i.value + q.offset;
	printf("surface: handle %u iova 0x%llx (info %d)\n", q.handle, (unsigned long long)i.value, r);
	uint32_t n = 0;
	cc[n++] = pkt7(CP_MEM_WRITE, 2 + N_WORDS);
	cc[n++] = (uint32_t)at;
	cc[n++] = (uint32_t)(at >> 32);
	for (int k = 0; k < N_WORDS; k++) {
		cc[n++] = 0x5f000000u | (uint32_t)k;
	}
	__sync_synchronize();
	struct drm_msm_gem_submit_bo bos[2] = {
		{ .flags = MSM_SUBMIT_BO_READ, .handle = hc, .presumed = ic },
		{ .flags = MSM_SUBMIT_BO_WRITE, .handle = q.handle, .presumed = i.value },
	};
	struct drm_msm_gem_submit_cmd cmd = { .type = MSM_SUBMIT_CMD_BUF, .submit_idx = 0, .size = n * 4 };
	struct drm_msm_gem_submit s = {
		.flags = 1, .nr_bos = 2, .nr_cmds = 1, .bos = (uintptr_t)bos, .cmds = (uintptr_t)&cmd, .queueid = queue,
	};
	r = call(DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT, &s, sizeof(s));
	struct drm_msm_wait_fence w = { .fence = s.fence, .timeout = { 1, 0 }, .queueid = queue };
	int rw = r ? -1 : call(DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, &w, sizeof(w));
	IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
	int bad = 0;
	for (int k = 0; k < N_WORDS; k++) {
		bad += px[k] != (0x5f000000u | (uint32_t)k);
	}
	printf("surface: submit %d wait %d: %d/%d words wrong, px[0] 0x%08x px[255] 0x%08x\n", r, rw, bad, N_WORDS,
	    px[0], px[N_WORDS - 1]);
	IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
	struct drm_gem_close c = { .handle = q.handle };
	call(DRM_GEM_CLOSE, &c, sizeof(c));
	CFRelease(surf);
	return r == 0 && rw == 0 && bad == 0;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);       /* every step reaches ssh even if the kernel dies next */
	printf("step: matching SweetGPU\n");
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetGPU"));
	if (svc == 0) {
		fprintf(stderr, "no SweetGPU service (boot with sweetgpu=2)\n");
		return 1;
	}
	printf("step: IOServiceOpen\n");
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS) {
		fprintf(stderr, "IOServiceOpen: 0x%x\n", kr);
		return 1;
	}

	/* 1. parameters */
	static const struct { uint32_t id; const char *name; } params[] = {
		{ 1, "GPU_ID" }, { 3, "CHIP_ID" }, { 2, "GMEM_SIZE" }, { 6, "GMEM_BASE" }, { 4, "MAX_FREQ" },
		{ 0x0e, "VA_START" }, { 0x0f, "VA_SIZE" }, { 7, "NR_RINGS" }, { 5, "TIMESTAMP" }, { 5, "TIMESTAMP" },
	};
	for (unsigned k = 0; k < sizeof(params) / sizeof(params[0]); k++) {
		struct drm_msm_param p = { .pipe = 1, .param = params[k].id };
		int r = call(DRM_COMMAND_BASE + DRM_MSM_GET_PARAM, &p, sizeof(p));
		printf("param %-10s %s 0x%llx (%llu)\n", params[k].name, r ? "ERR" : "=", (unsigned long long)p.value,
		    (unsigned long long)p.value);
	}
	struct drm_msm_submitqueue q = { 0 };
	int r = call(DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_NEW, &q, sizeof(q));
	printf("submitqueue: %d id %u\n", r, q.id);

	/* 2. buffers */
	printf("step: GEM_NEW + GEM_INFO + map\n");
	uint32_t hc, hd;
	uint64_t ic, id;
	volatile uint32_t *cc, *cd;
	if (bo_new(16384, &hc, &ic, &cc) != 0 || bo_new(16384, &hd, &id, &cd) != 0) {
		fprintf(stderr, "buffer allocation/mapping failed\n");
		return 1;
	}
	printf("buffers: cmd handle %u iova 0x%llx cpu %p, data handle %u iova 0x%llx cpu %p\n", hc,
	    (unsigned long long)ic, (void *)cc, hd, (unsigned long long)id, (void *)cd);
	for (int k = 0; k < N_WORDS; k++) {
		cd[k] = 0xdeadbeef;
	}

	/* 3. command buffer */
	uint32_t n = 0;
	cc[n++] = pkt7(CP_MEM_WRITE, 2 + N_WORDS);
	cc[n++] = (uint32_t)id;
	cc[n++] = (uint32_t)(id >> 32);
	for (int k = 0; k < N_WORDS; k++) {
		cc[n++] = 0xc0de0000u | (uint32_t)k;
	}
	__sync_synchronize();

	/* 4. submit, wait, check */
	printf("step: SUBMIT\n");
	struct drm_msm_gem_submit_bo bos[2] = {
		{ .flags = MSM_SUBMIT_BO_READ, .handle = hc, .presumed = ic },
		{ .flags = MSM_SUBMIT_BO_WRITE, .handle = hd, .presumed = id },
	};
	struct drm_msm_gem_submit_cmd cmd = { .type = MSM_SUBMIT_CMD_BUF, .submit_idx = 0, .size = n * 4 };
	struct drm_msm_gem_submit s = {
		.flags = 1, .nr_bos = 2, .nr_cmds = 1, .bos = (uintptr_t)bos, .cmds = (uintptr_t)&cmd,
		.queueid = q.id,
	};
	uint64_t t0 = now_ns();
	r = call(DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT, &s, sizeof(s));
	struct drm_msm_wait_fence w = { .fence = s.fence, .timeout = { 1, 0 }, .queueid = q.id };
	int rw = r ? -1 : call(DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, &w, sizeof(w));
	struct drm_msm_gem_cpu_prep cp = { .handle = hd, .op = MSM_PREP_READ, .timeout = { 1, 0 } };
	int rp = r ? -1 : call(DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_PREP, &cp, sizeof(cp));
	uint64_t t1 = now_ns();
	int bad = 0;
	for (int k = 0; k < N_WORDS; k++) {
		bad += cd[k] != (0xc0de0000u | (uint32_t)k);
	}
	printf("submit %d (fence %u), wait %d, cpu_prep %d, %llu us: %d/%d words wrong, data[0] 0x%08x data[255] 0x%08x\n",
	    r, s.fence, rw, rp, (unsigned long long)((t1 - t0) / 1000), bad, N_WORDS, cd[0], cd[N_WORDS - 1]);

	/* 5. latency */
	if (r == 0 && rw == 0 && bad == 0) {
		uint64_t b0 = now_ns();
		int fails = 0;
		for (int k = 0; k < 200; k++) {
			s.fence = 0;
			if (call(DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT, &s, sizeof(s)) != 0) {
				fails++;
				continue;
			}
			w.fence = s.fence;
			fails += call(DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, &w, sizeof(w)) != 0;
		}
		printf("200 submit+wait: %llu us each, %d failures, last fence %u\n",
		    (unsigned long long)((now_ns() - b0) / 200000), fails, s.fence);
	}

	/* 6. an IOSurface as a buffer */
	int surface_ok = -1;                    /* -1: not tried / not supported by this kernel */
	if (r == 0 && rw == 0 && bad == 0) {
		surface_ok = surface_test(hc, ic, cc, q.id);
	}

	struct drm_gem_close c1 = { .handle = hc }, c2 = { .handle = hd };
	call(DRM_GEM_CLOSE, &c1, sizeof(c1));
	call(DRM_GEM_CLOSE, &c2, sizeof(c2));
	IOServiceClose(conn);
	int ok = r == 0 && rw == 0 && bad == 0;
	printf("RESULT %s (surface: %s)\n", ok ? "OK" : "FAIL",
	    surface_ok < 0 ? "not tried/unsupported" : surface_ok ? "OK" : "FAIL");
	return ok ? 0 : 1;
}
