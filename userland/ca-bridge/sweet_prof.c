/*
 * (op5) A sampling profiler for the thread that draws (backboardd's render thread): where its
 * processor time goes, by library and code offset. On when /usr/local/lib/glprof exists at load.
 *
 * A thread of ours wakes every PROF_PERIOD_US, suspends the drawing thread, reads its pc (and
 * lr, for leaf functions), resumes it, and counts the pc in PROF_BUCKET-byte buckets per image.
 * Every PROF_REPORT samples: one "prof:" line per image with its share, then the top buckets as
 * "prof: IMAGE+0xOFFSET N" for symbolication on the Mac (atos -o UNSTRIPPED -l 0 OFFSET).
 * Images are looked up with dladdr after the thread is resumed (it may hold dyld's lock).
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sweet_prof.h"

#define PROF_PERIOD_US  1000
#define PROF_REPORT     4000
#define PROF_BUCKET     64
#define PROF_SLOTS      4096
#define PROF_TOP        40
#define PROF_IMAGES     32

struct prof_slot {
	uintptr_t base, offset;         /* image base and bucket offset in it; count 0 = empty */
	const char *image;
	uint32_t count;
};

static struct prof_slot slots[PROF_SLOTS];
static struct {
	const char *name;
	uint32_t count;
} images[PROF_IMAGES];
static _Atomic mach_port_t target;
static uint32_t samples, lost;

static void
prof_say(const char *line, size_t n)
{
	write(2, line, n);
}

static const char *
short_name(const char *path)
{
	const char *slash = path != NULL ? strrchr(path, '/') : NULL;

	return slash != NULL ? slash + 1 : path != NULL ? path : "?";
}

static void
count_image(const char *name)
{
	for (unsigned i = 0; i < PROF_IMAGES; i++) {
		if (images[i].name == name || images[i].name == NULL) {
			images[i].name = name;
			images[i].count++;
			return;
		}
	}
}

static void
count_pc(uintptr_t pc)
{
	Dl_info info;
	const char *image = "?";
	uintptr_t base = 0;

	if (dladdr((const void *)pc, &info) != 0) {
		image = short_name(info.dli_fname);
		base = (uintptr_t)info.dli_fbase;
	}
	count_image(image);
	uintptr_t offset = (pc - base) & ~(uintptr_t)(PROF_BUCKET - 1);
	uintptr_t hash = (base ^ offset) * 2654435761u;
	for (unsigned probe = 0; probe < PROF_SLOTS; probe++) {
		struct prof_slot *s = &slots[(hash + probe) % PROF_SLOTS];

		if (s->count != 0 && s->base == base && s->offset == offset) {
			s->count++;
			return;
		}
		if (s->count == 0) {
			s->base = base;
			s->offset = offset;
			s->image = image;
			s->count = 1;
			return;
		}
	}
	lost++;
}

static void
report(void)
{
	char line[256];
	int n;

	n = snprintf(line, sizeof(line), "prof: %u samples (%u lost)\n", samples, lost);
	prof_say(line, (size_t)n);
	for (unsigned i = 0; i < PROF_IMAGES && images[i].name != NULL; i++) {
		if (images[i].count * 200u >= samples) {
			n = snprintf(line, sizeof(line), "prof: image %s %u.%u%%\n", images[i].name,
			    images[i].count * 100u / samples, images[i].count * 1000u / samples % 10);
			prof_say(line, (size_t)n);
		}
	}
	for (unsigned t = 0; t < PROF_TOP; t++) {
		struct prof_slot *best = NULL;

		for (unsigned i = 0; i < PROF_SLOTS; i++) {
			if (slots[i].count != 0 && (best == NULL || slots[i].count > best->count)) {
				best = &slots[i];
			}
		}
		if (best == NULL) {
			break;
		}
		n = snprintf(line, sizeof(line), "prof: top %s+%#lx %u\n", best->image, (unsigned long)best->offset,
		    best->count);
		prof_say(line, (size_t)n);
		best->count = 0;        /* (taken; the table is cleared below anyway) */
	}
	memset(slots, 0, sizeof(slots));
	memset(images, 0, sizeof(images));
	samples = lost = 0;
}

/*
 * Every report: each thread of the task, its processor time (user, system) since the last
 * report, the busiest few named; the busiest becomes the one sampled.
 */
#define PROF_THREADS 256
#define PROF_SHOWN 6
static struct {
	uint64_t id, user_us, system_us;
} seen[PROF_THREADS];
static mach_port_t sampler_port;

struct thread_use {
	mach_port_t port;
	uint64_t user_us, system_us;    /* since the last report */
	char name[64];
};

static uint64_t
time_us(time_value_t t)
{
	return (uint64_t)t.seconds * 1000000u + (uint64_t)t.microseconds;
}

/* One thread's use since the last report; false if it cannot be asked. */
static bool
thread_use_of(mach_port_t port, struct thread_use *use)
{
	thread_basic_info_data_t basic;
	thread_identifier_info_data_t ident;
	thread_extended_info_data_t ext;
	mach_msg_type_number_t c1 = THREAD_BASIC_INFO_COUNT, c2 = THREAD_IDENTIFIER_INFO_COUNT,
	    c3 = THREAD_EXTENDED_INFO_COUNT;

	if (thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&basic, &c1) != KERN_SUCCESS ||
	    thread_info(port, THREAD_IDENTIFIER_INFO, (thread_info_t)&ident, &c2) != KERN_SUCCESS) {
		return false;
	}
	ext.pth_name[0] = 0;
	(void)thread_info(port, THREAD_EXTENDED_INFO, (thread_info_t)&ext, &c3);
	uint64_t user = time_us(basic.user_time), system = time_us(basic.system_time);
	unsigned slot = (unsigned)(ident.thread_id % PROF_THREADS);
	uint64_t user_before = 0, system_before = 0;
	if (seen[slot].id == ident.thread_id) {
		user_before = seen[slot].user_us;
		system_before = seen[slot].system_us;
	}
	seen[slot].id = ident.thread_id;
	seen[slot].user_us = user;
	seen[slot].system_us = system;
	use->port = port;
	use->user_us = user - user_before;
	use->system_us = system - system_before;
	snprintf(use->name, sizeof(use->name), "%s", ext.pth_name[0] != 0 ? ext.pth_name : "-");
	return true;
}

static void
threads_report(void)
{
	thread_act_array_t list;
	mach_msg_type_number_t count = 0;
	struct thread_use top[PROF_SHOWN + 1];
	unsigned shown = 0;
	char line[256];
	int n;

	if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) {
		return;
	}
	for (mach_msg_type_number_t i = 0; i < count; i++) {
		struct thread_use use;

		if (list[i] == sampler_port || !thread_use_of(list[i], &use)) {
			continue;
		}
		unsigned at = shown;
		while (at > 0 && top[at - 1].user_us + top[at - 1].system_us < use.user_us + use.system_us) {
			at--;
		}
		if (at < PROF_SHOWN) {
			memmove(&top[at + 1], &top[at], sizeof(top[0]) * (shown - at));
			top[at] = use;
			shown += shown < PROF_SHOWN;
		}
	}
	for (unsigned t = 0; t < shown; t++) {
		n = snprintf(line, sizeof(line), "prof: thread %s: user %llu ms, system %llu ms%s\n", top[t].name,
		    (unsigned long long)(top[t].user_us / 1000), (unsigned long long)(top[t].system_us / 1000),
		    t == 0 ? " (sampled next)" : "");
		prof_say(line, (size_t)n);
	}
	if (shown > 0) {
		atomic_store(&target, top[0].port);
	}
	/* the ports stay referenced (a few dozen send rights a report; sampling is a test mode) */
	vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(list[0]));
}

static void *
sampler(void *argument)
{
	(void)argument;
	sampler_port = pthread_mach_thread_np(pthread_self());
	threads_report();
	for (;;) {
		usleep(PROF_PERIOD_US);
		mach_port_t thread = atomic_load(&target);
		if (thread == MACH_PORT_NULL) {
			continue;
		}
		arm_thread_state64_t state;
		mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
		if (thread_suspend(thread) != KERN_SUCCESS) {
			continue;
		}
		kern_return_t got = thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, &count);
		thread_resume(thread);
		if (got != KERN_SUCCESS) {
			continue;
		}
		uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(state);
		samples++;
		count_pc(pc);
		if (samples >= PROF_REPORT) {
			report();
			threads_report();
		}
	}
	return NULL;
}

void
sweet_prof_start(void)
{
	pthread_t thread;

	if (access("/usr/local/lib/glprof", F_OK) != 0) {
		return;
	}
	if (pthread_create(&thread, NULL, sampler, NULL) == 0) {
		pthread_detach(thread);
		prof_say("prof: sampling the drawing thread every 1 ms\n", 46);
	}
}

void
sweet_prof_thread(void)
{
	static _Thread_local int noted;

	if (!noted) {
		noted = 1;
		atomic_store(&target, pthread_mach_thread_np(pthread_self()));
	}
}
