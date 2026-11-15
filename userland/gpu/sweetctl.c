/*
 * sweetctl: read or write an integer kernel sysctl by name (toybox's sysctl
 * only knows Linux /proc/sys).
 *
 *   sweetctl kern.sweetgpu_stage        print the value
 *   sweetctl kern.sweetgpu_stage 1      write 1, then print the value again
 *
 * Build: xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 \
 *        -o sweetctl sweetctl.c   (then ad-hoc sign like the other tools)
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

static int
read_int(const char *name, int *out)
{
	size_t len = sizeof(*out);
	if (sysctlbyname(name, out, &len, NULL, 0) != 0) {
		fprintf(stderr, "sweetctl: read %s: %s\n", name, strerror(errno));
		return -1;
	}
	if (len != sizeof(*out)) {
		fprintf(stderr, "sweetctl: %s is not an int (%zu bytes)\n", name, len);
		return -1;
	}
	return 0;
}

static int
parse_int(const char *text, int *out)
{
	char *end = NULL;
	errno = 0;
	long v = strtol(text, &end, 0);
	if (errno != 0 || end == text || *end != '\0' || v < INT_MIN || v > INT_MAX) {
		fprintf(stderr, "sweetctl: not an integer: %s\n", text);
		return -1;
	}
	*out = (int)v;
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc != 2 && argc != 3) {
		fprintf(stderr, "usage: sweetctl <name> [int value]\n");
		return 2;
	}
	const char *name = argv[1];
	if (argc == 3) {
		int value = 0;
		if (parse_int(argv[2], &value) != 0) {
			return 2;
		}
		if (sysctlbyname(name, NULL, NULL, &value, sizeof(value)) != 0) {
			fprintf(stderr, "sweetctl: write %s=%d: %s\n", name, value, strerror(errno));
			return 1;
		}
	}
	int now = 0;
	if (read_int(name, &now) != 0) {
		return 1;
	}
	printf("%s: %d\n", name, now);
	return 0;
}
