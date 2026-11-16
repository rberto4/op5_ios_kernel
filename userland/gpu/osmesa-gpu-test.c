/*
 * osmesa-gpu-test: first rendering by Mesa's freedreno on the A540 (phase E step 2).
 *
 * OSMesa (userland/mesa, Mesa 25.0.7 patched to open the kernel's GPU
 * through redwood-drm) renders into a 256x256 buffer: clear to blue, a
 * red triangle, glFinish; then the CPU checks the pixels and times frames.
 * OSMESA_SOFTWARE=1 forces softpipe for comparison.
 *
 * Build (see userland/gpu/build-osmesa-test.sh): links libOSMesa.8.dylib,
 * which goes to /usr/local/lib on the phone.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <GL/osmesa.h>
#include <GL/gl.h>

#define W 256
#define H 256

static uint64_t
now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void
draw(void)
{
	glViewport(0, 0, W, H);
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glBegin(GL_TRIANGLES);
	glColor3f(1.0f, 0.0f, 0.0f);
	glVertex2f(-0.5f, -0.5f);
	glVertex2f(0.5f, -0.5f);
	glVertex2f(0.0f, 0.5f);
	glEnd();
	glFinish();
}

int
main(void)
{
	uint32_t *buf = calloc(W * H, 4);
	OSMesaContext ctx = OSMesaCreateContextExt(OSMESA_RGBA, 24, 0, 0, NULL);
	if (ctx == NULL || buf == NULL) {
		fprintf(stderr, "OSMesaCreateContextExt failed\n");
		return 1;
	}
	if (!OSMesaMakeCurrent(ctx, buf, GL_UNSIGNED_BYTE, W, H)) {
		fprintf(stderr, "OSMesaMakeCurrent failed\n");
		return 1;
	}
	printf("GL_VENDOR   %s\nGL_RENDERER %s\nGL_VERSION  %s\n", glGetString(GL_VENDOR),
	    glGetString(GL_RENDERER), glGetString(GL_VERSION));

	uint64_t t0 = now_us();
	draw();
	uint64_t first = now_us() - t0;

	/* OSMesa rows are bottom-up: row 0 is the bottom of the picture */
	uint32_t corner = buf[0], centre = buf[(H / 2) * W + W / 2];
	int blue = 0, red = 0, other = 0;
	for (int i = 0; i < W * H; i++) {
		uint32_t p = buf[i] & 0x00ffffffu;      /* RGBA in memory: R lowest byte */
		if (p == 0x00ff0000u) {
			blue++;
		} else if (p == 0x000000ffu) {
			red++;
		} else {
			other++;
		}
	}
	printf("first frame %llu us: corner 0x%08x centre 0x%08x; blue %d red %d other %d\n",
	    (unsigned long long)first, corner, centre, blue, red, other);

	t0 = now_us();
	for (int i = 0; i < 100; i++) {
		draw();
	}
	printf("100 frames: %llu us each\n", (unsigned long long)((now_us() - t0) / 100));

	/* a triangle of half the width and height covers 1/8 of the pixels: ~8192 */
	int ok = blue > 40000 && red > 6000 && red < 10000;
	printf("RESULT %s\n", ok ? "OK" : "FAIL");
	OSMesaDestroyContext(ctx);
	return ok ? 0 : 1;
}
