/*
 * blkapply: apply a block patch to a file in place (for /data/ios/root.img
 * from TWRP). Never truncates, never extends, never moves the file's
 * extents: every write is a pwrite of one 4096-byte block inside the file.
 *
 *   blkapply FILE PATCH          verify, then apply
 *   blkapply -n FILE PATCH       verify only
 *
 * PATCH (from tools/blkdiff.py): "OP5BLKP1" | u64 file size | u64 count,
 * then count records of u64 offset | 4096 old bytes | 4096 new bytes
 * (little endian). Phase 1 checks the file size and that every block holds
 * either its OLD or its NEW content (NEW = already applied, skipped); any
 * other content aborts before anything is written. Phase 2 writes, fsyncs
 * and reads every written block back.
 *
 * Build: zig cc -target aarch64-linux-musl -static -O2 -Wall -Wextra -s
 */
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLK 4096u

struct rec {
	uint64_t off;
	unsigned char old[BLK];
	unsigned char new[BLK];
};

static int
read_full(int fd, void *buf, size_t n)
{
	size_t done = 0;
	while (done < n) {
		ssize_t r = read(fd, (char *)buf + done, n - done);
		if (r <= 0)
			return -1;
		done += (size_t)r;
	}
	return 0;
}

static int
pread_full(int fd, void *buf, size_t n, off_t off)
{
	size_t done = 0;
	while (done < n) {
		ssize_t r = pread(fd, (char *)buf + done, n - done, off + (off_t)done);
		if (r <= 0)
			return -1;
		done += (size_t)r;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int dry = 0;
	if (argc == 4 && strcmp(argv[1], "-n") == 0) {
		dry = 1;
		argv++;
		argc--;
	}
	if (argc != 3) {
		fprintf(stderr, "usage: blkapply [-n] FILE PATCH\n");
		return 2;
	}
	int pf = open(argv[2], O_RDONLY);
	if (pf < 0) {
		perror(argv[2]);
		return 1;
	}
	char magic[8];
	uint64_t size, count;
	if (read_full(pf, magic, 8) || memcmp(magic, "OP5BLKP1", 8) ||
	    read_full(pf, &size, 8) || read_full(pf, &count, 8) || count > (1u << 20)) {
		fprintf(stderr, "bad patch header\n");
		return 1;
	}
	struct rec *recs = calloc(count ? count : 1, sizeof(*recs));
	if (!recs) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}
	for (uint64_t i = 0; i < count; i++) {
		if (read_full(pf, &recs[i], 8 + 2 * BLK)) {
			fprintf(stderr, "short patch at record %llu\n", (unsigned long long)i);
			return 1;
		}
		if (recs[i].off % BLK || recs[i].off + BLK > size) {
			fprintf(stderr, "record %llu: bad offset %llu\n", (unsigned long long)i,
			    (unsigned long long)recs[i].off);
			return 1;
		}
	}
	close(pf);

	int fd = open(argv[1], dry ? O_RDONLY : O_RDWR);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	struct stat st;
	if (fstat(fd, &st) || (uint64_t)st.st_size != size) {
		fprintf(stderr, "file size %lld, patch expects %llu: refusing\n",
		    (long long)st.st_size, (unsigned long long)size);
		return 1;
	}
	unsigned char *cur = malloc(BLK);
	char *todo = calloc(count ? count : 1, 1);
	uint64_t pending = 0, applied = 0;
	for (uint64_t i = 0; i < count; i++) {
		if (pread_full(fd, cur, BLK, (off_t)recs[i].off)) {
			fprintf(stderr, "read error at %llu\n", (unsigned long long)recs[i].off);
			return 1;
		}
		if (memcmp(cur, recs[i].old, BLK) == 0) {
			todo[i] = 1;
			pending++;
		} else if (memcmp(cur, recs[i].new, BLK) == 0) {
			applied++;
		} else {
			fprintf(stderr, "block at %llu is neither OLD nor NEW: refusing, nothing written\n",
			    (unsigned long long)recs[i].off);
			return 1;
		}
	}
	printf("verify: %llu blocks, %llu to write, %llu already new\n",
	    (unsigned long long)count, (unsigned long long)pending, (unsigned long long)applied);
	if (dry || !pending)
		return 0;
	for (uint64_t i = 0; i < count; i++) {
		if (!todo[i])
			continue;
		if (pwrite(fd, recs[i].new, BLK, (off_t)recs[i].off) != (ssize_t)BLK) {
			fprintf(stderr, "write error at %llu\n", (unsigned long long)recs[i].off);
			return 1;
		}
	}
	if (fsync(fd)) {
		perror("fsync");
		return 1;
	}
	for (uint64_t i = 0; i < count; i++) {
		if (pread_full(fd, cur, BLK, (off_t)recs[i].off) || memcmp(cur, recs[i].new, BLK)) {
			fprintf(stderr, "read-back mismatch at %llu\n", (unsigned long long)recs[i].off);
			return 1;
		}
	}
	if (fstat(fd, &st) || (uint64_t)st.st_size != size) {
		fprintf(stderr, "size changed!\n");
		return 1;
	}
	printf("applied %llu blocks, read back OK, size %llu unchanged\n",
	    (unsigned long long)pending, (unsigned long long)size);
	return 0;
}
