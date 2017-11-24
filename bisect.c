#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>

/* Returns the file offset of the line starting at ofs, or if no line
 * starts their, then the the offset of the next line.
 */
static off_t get_fofs(char *file, size_t size, off_t ofs) {
  int c;
  assert(ofs >= 0);
  if (ofs == 0) return 0;
  if (ofs > size) return size;
  --ofs;
  for (;;) {
    if (size == ofs) return ofs;
    c = *(file + ofs);
    ++ofs;
    if (c == '\n') {
	int i = strcspn(file + ofs, "\n");
	char *t = strndupa(file + ofs, i);
	printf("%s\n", t);
    	return ofs;
    }
  }
}

/* Compares x[:xsize] with a line read from yf. */
static bool compare_line(char *file, size_t size, off_t fofs,
                          const char *x, size_t xsize) {
	unsigned int b, c;
	if (fofs == size) return true;  /* Special casing of EOF at BOL. */
  for (;;) {
    c = *(file + fofs++);
    if (c == '\n') {
      return true;
    } else if (xsize == 0) {
      return false;
    } else if ((b = *(unsigned char*)x) != c) {
      return b < c;
    }
    ++x;
    --xsize;
  }
}

static off_t bisect_way(
	char *file, size_t size,
	off_t lo, off_t hi,
	const char *x, size_t xsize) {
	off_t mid, midf;
	if (hi + 0ULL > size + 0ULL)
		hi = size;  /* Also applies to hi == -1. */
	if (xsize == 0) {  /* Shortcuts. */
		if (hi == size) return hi;
	}
	if (lo >= hi) return get_fofs(file, size, lo);
	do {
		mid = (lo + hi) >> 1;
		midf = get_fofs(file, size, mid);
		if (compare_line(file, size, mid, x, xsize)) {
			hi = mid;
		} else {
			lo = mid + 1;
		}
	} while (lo < hi);
	return mid == lo ? midf : get_fofs(file, size, lo);
}

int main(int argc, char *argv[]) {
	char *file;
	size_t size;
	int fd = -1;
	struct stat st;
	int r;
	char *search = "gcc\377";

	fd = open("/var/cache/command-not-found/db", O_RDONLY);
	if (fd < 0)
		abort();
	r = fstat(fd, &st);
	if (r < 0)
		abort();
	size = st.st_size;
	file = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	
	r = bisect_way(file, size, 0, (off_t)-1, search, strlen(search));
	
}
