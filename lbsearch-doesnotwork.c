#define DUMMY \
  set -ex; ${CC:-gcc} -ansi -W -Wall -Wextra -Werror=missing-declarations \
      -s -O2 -DNDEBUG -o pts_lbsearch "$0"; : OK; exit
/*
 * pts_lbsearch.c: Fast binary search in a line-sorted text file.
 * by pts@fazekas.hu at Sat Nov 30 02:42:03 CET 2013
 *
 * License: GNU GPL v2 or newer, at your choice.
 *
 * Please note that ordering is the lexicographical order of the byte
 * strings within the input text file, and the byte 10 (LF, '\n') is used as
 * terminator (no CR, \r). If the input file is not sorted, pts_lbsearch.c
 * won't crash, but the results will be incorrect. On Unix, use
 * `LC_CTYPE=C sort <file >file.sorted' to sort files. Without LC_CTYPE=C,
 * sort will use the locale's sort order, which may not be lexicographical if
 * there are non-ASCII characters in the file.
 *
 * The line buffering code in the binary search implementation in this file
 * is very tricky. See (ARTICLE)
 * http://pts.github.io/pts-line-bisect/line_bisect_evolution.html for a
 * detailed explananation, containing the design and analysis of the
 * algorithms implemented in this file.
 *
 * Nice properties of this implementation:
 *
 * * no dynamic memory allocation (except possibly for stdio.h)
 * * no unnecessary lseek(2) or read(2) system calls
 * * no unnecessary comparisons for long strings
 * * very small memory usage: only a few dozen of offsets and flags in addition
 *   to a single file read buffer (of 8K by default)
 * * no printf
 * * compiles without warnings in C and C++
 *   (gcc -std=c89; gcc -std=c99; gcc -std=c11;
 *   gcc -ansi; g++ -std=c++98; g++ -std=c++11; g++ -std=ansi; also
 *   correspondingly with clang and clang++).
 *
 * -Werror=implicit-function-declaration is not supported by gcc-4.1.
 *
 * TODO(pts): Add flag `-aq' for only CM_LT offset.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef STATIC
#define STATIC static
#endif

typedef char ybool;

typedef struct yfile {
  char *mmap;
  off_t size;
  off_t ofs;
} yfile;

struct cache_entry {
  off_t ofs;
  off_t fofs;
  ybool cmp_result;
};

STATIC __attribute__((noreturn)) void die5_code(
    const char *msg1, const char *msg2, const char *msg3, const char *msg4,
    const char *msg5, int exit_code) {
  const size_t msg1_size = strlen(msg1), msg2_size = strlen(msg2);
  const size_t msg3_size = strlen(msg3), msg4_size = strlen(msg4);
  const size_t msg5_size = strlen(msg5);  /* !! */
  (void)!write(STDERR_FILENO, msg1, msg1_size);
  (void)!write(STDERR_FILENO, msg2, msg2_size);
  (void)!write(STDERR_FILENO, msg3, msg3_size);
  (void)!write(STDERR_FILENO, msg4, msg4_size);
  (void)!write(STDERR_FILENO, msg5, msg5_size);
  exit(exit_code);
}

STATIC __attribute__((noreturn)) void die2_strerror(
    const char *msg1, const char *msg2) {
  die5_code(msg1, msg2, ": ", strerror(errno), "\n", 2);
}

STATIC __attribute__((noreturn)) void die1(const char *msg1) {
  die5_code(msg1, "", "", "", "\n", 2);
}

/** Constructor. Opens and initializes yf.
 * If size != (off_t)-1, then it will be imposed as a limit.
 */
STATIC void yfopen(yfile *yf, const char *pathname, off_t size) {
  int fd = open(pathname, O_RDONLY, 0);
  if (fd < 0) {
    die2_strerror("error: open ", pathname);
    exit(2);
  }
  if (size == -1) {
    size = lseek(fd, 0, SEEK_END);
    if (size + 1ULL == 0ULL) {
      if (errno == ESPIPE) {
        die1("error: input not seekable, cannot binary search");
      } else {
        die2_strerror("error: lseek end", "");
      }
    }
  }
  void *m = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  if (!m) {
        die2_strerror("error: mmap ", pathname);
        exit(2);
  }
  yf->mmap = m;
  close(fd);
  yf->size = size;
}

STATIC void yfclose(yfile *yf) {
  munmap(yf->mmap, yf->size);
  yf->size = 0;
}

STATIC off_t yfgetsize(yfile *yf) {
  return yf->size;
}

/* --- Bisection (binary search)
 *
 * The algorithms and data structures below are complex, tricky, and very
 * underdocumented. See (ARTICLE) above for a detailed explanation of both
 * design and implementation.
 */

/* Returns the file offset of the line starting at ofs, or if no line
 * starts their, then the the offset of the next line.
 */
STATIC off_t get_fofs(yfile *yf, off_t ofs) {
  int c;
  off_t size;
  assert(ofs >= 0);
  if (ofs == 0) return 0;
  size = yf->size;
  if (ofs > size) return size;
  --ofs;
  yf->ofs = ofs;
  do {
    c = *(yf->mmap + ofs);
    ++ofs;
  } while (c != '\n');
  return ofs;
}

typedef enum compare_mode_t {
  CM_LE,  /* True iff x <= y (where y is read from the file). */
  CM_LT,  /* True iff x < y. */
  CM_LP,  /* x* < y, where x* is x + a fake byte 256 and the end. */
  CM_UNSET,  /* Not set yet. Most functions do not support it. */
} compare_mode_t;

/* Compares x[:xsize] with a line read from yf. */
STATIC ybool compare_line(yfile *yf, off_t fofs,
                          const char *x, size_t xsize, compare_mode_t cm) {
  int b, c;
  yf->ofs = fofs;
  if (yf->ofs == yf->size) return 1;  /* Special casing of EOF at BOL. */
  /* TODO(pts): Possibly speed up the loop with yfpeek(...). */
  for (;;) {
    c = *(yf->mmap + yf->ofs);
    if (c < 0 || c == '\n') {
      return cm == CM_LE ? xsize == 0 : 0;
    } else if (xsize == 0) {
      return cm != CM_LP;
    } else if ((b = (int)*(unsigned char*)x) != c) {
      return b < c;
    }
    ++x;
    --xsize;
  }
}

/* x[:xsize] must not contain '\n'.
 *
 * cm=CM_LE is equivalent to is_left=true and is_open=true.
 * cm=CM_LT is equivalent to is_left=false and is_open=false.
 * cm=CL_LP is also supported, it does prefix search.
 */
STATIC off_t bisect_way(
    yfile *yf, off_t lo, off_t hi,
    const char *x, size_t xsize, compare_mode_t cm) {
  const off_t size = yfgetsize(yf);
  off_t mid, midf;
  struct cache_entry entry;
  if (hi + 0ULL > size + 0ULL) hi = size;  /* Also applies to hi == -1. */
  if (xsize == 0) {  /* Shortcuts. */
    if (cm == CM_LE) hi = lo;  /* Faster for lo == 0. Returns right below. */
    if (cm == CM_LP && hi == size) return hi;
  }
  if (lo >= hi) return yf->ofs;
  do {
    mid = (lo + hi) >> 1;
    entry.fofs = get_fofs(yf, mid);
    entry.ofs = mid;
    entry.cmp_result = compare_line(yf, entry.fofs, x, xsize, cm);
    midf = entry.fofs;
    if (entry.cmp_result) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  } while (lo < hi);
  return mid == lo ? midf : get_fofs(yf, lo);
}

/* x[:xsize] and y[:ysize] must not contain '\n'. */
STATIC void bisect_interval(
    yfile *yf, off_t lo, off_t hi, compare_mode_t cm,
    const char *x, size_t xsize,
    const char *y, size_t ysize,
    off_t *start_out, off_t *end_out) {
  off_t start;
  *start_out = start = bisect_way(yf, lo, hi, x, xsize, CM_LE);
  if (cm == CM_LE && xsize == ysize && 0 == memcmp(x, y, xsize)) {
    *end_out = start;
  } else {
    *end_out = bisect_way(yf, start, hi, y, ysize, cm);
  }
}

/* --- main */

STATIC __attribute__((noreturn)) void usage_error(
    const char *argv0, const char *msg) {
  die5_code("Binary search (bisection) in a sorted text file\n"
            "Usage: ", argv0, "-<flags> <sorted-text-file> <key-x> [<key-y>]\n"
            "<key-x> is the first key to search for\n"
            "<key-y> is the last key to search for; default is <key-x>\n"
            "Flags:\n"
            "e: do bisect_left, open interval end\n"
            "t: do bisect_right, closed interval end\n"
            "b: do bisect_left for interval start (default)\n"
            "a: do bisect_right for interval start (for append position)\n"
            "p: do prefix search\n"
            "c: print file contents (default)\n"
            "o: print file offsets\n"
            "q: don't print anything, just detect if there is a match\n"
            "i: ignore incomplete last line (may be appended to right now)\n"
            "usage error: ", msg, "\n",
            1);
}

STATIC void write_all_to_stdout(const char *buf, size_t size) {
  size_t got = write(STDOUT_FILENO, buf, size);
  if (got == size) {
  } else if (got + 1U == 0U) {
   die2_strerror("error: write stdout", "");
  } else {
   die1("error: short write");
  }
}

STATIC void print_range(yfile *yf, off_t start, off_t end) {
  write_all_to_stdout(yf->mmap + start, end - start);
}

#if defined(__i386__) && __SIZEOF_INT__ == 4 && __SIZEOF_LONG_LONG__ == 8 && \
    defined(__GNUC__)
/* A smaller implementation of division for format_unsigned, which doesn't
 * call the __udivdi3 (for 64-bit /) and __umoddi3 (for 64-bit %) functions
 * from libgcc. This implementation can be smaller because the divisor (b) is
 * only 32 bits.
 */

typedef unsigned UInt32;
typedef unsigned long long UInt64;
/* Returns *a % b, and sets *a = *a_old / b; */
STATIC __inline__ UInt32 UInt64DivAndGetMod(UInt64 *a, UInt32 b) {
  /* http://stackoverflow.com/a/41982320/97248 */
  UInt32 upper = ((UInt32*)a)[1], r;
  ((UInt32*)a)[1] = 0;
  if (upper >= b) {
    ((UInt32*)a)[1] = upper / b;
    upper %= b;
  }
  __asm__("divl %2" : "=a" (((UInt32*)a)[0]), "=d" (r) :
      "rm" (b), "0" (((UInt32*)a)[0]), "1" (upper));
  return r;
}
/* Returns *a % b, and sets *a = *a_old / b; */
STATIC __inline__ UInt32 UInt32DivAndGetMod(UInt32 *a, UInt32 b) {
  /* gcc-4.4 is smart enough to optimize the / and % to a single divl. */
  const UInt32 r = *a % b;
  *a /= b;
  return r;
}

/** Returns p + size of formatted output. */
STATIC char *format_unsigned(char *p, off_t i) {
  struct AssertOffTSizeIs4or8_Struct {
    int  AssertOffTSizeIs4or8 : sizeof(i) == 4 || sizeof(i) == 8;
  };
  char *q = p, *result, c;
  assert(i >= 0);
  do {
    *q++ = '0' + (
        sizeof(i) == 8 ? UInt64DivAndGetMod((UInt64*)&i, 10) :
        sizeof(i) == 4 ? UInt32DivAndGetMod((UInt32*)&i, 10) :
        0);  /* 0 never happens, see AssertOffTSizeIs4or8 above. */
  } while (i != 0);
  result = q--;
  while (p < q) {  /* Reverse the string between p and q. */
    c = *p; *p++ = *q; *q-- = c;
  }
  return result;
}
#else

/** Returns p + size of formatted output. */
static char *format_unsigned(char *p, off_t i) {
  char *q = p, *result, c;
  assert(i >= 0);
  do {
    *q++ = '0' + (i % 10);
  } while ((i /= 10) != 0);
  result = q--;
  while (p < q) {  /* Reverse the string between p and q. */
    c = *p; *p++ = *q; *q-- = c;
  }
  return result;
}
#endif

typedef enum printing_t {
  PR_OFFSETS,
  PR_CONTENTS,
  PR_DETECT,
  PR_UNSET,
} printing_t;

typedef enum incomplete_t {
  IN_IGNORE,  /* Ignore incomplete last line of file. */
  IN_USE,  /* Use incomplete last line of file as if it had a trailin '\n'. */
  IN_UNSET,  /* Not set yet. Most functions do not support it. */
} incomplete_t;

int main(int argc, char **argv) {
  yfile yff, *yf = &yff;
  const char *x;
  const char *y;
  const char *filename;
  const char *flags;
  const char *p;
  char flag;
  /* Large enough to hold 2 off_t()s and 2 more bytes. */
  char ofsbuf[sizeof(off_t) * 6 + 2], *ofsp;
  compare_mode_t cm = CM_UNSET;
  compare_mode_t cmstart = CM_UNSET;
  size_t xsize, ysize;
  off_t start, end;
  printing_t printing = PR_UNSET;
  incomplete_t incomplete = IN_UNSET;

  /* Parse the command-line. */
  if (argc != 4 && argc != 5) usage_error(argv[0], "incorrect argument count");
  if (argv[1][0] != '-') usage_error(argv[0], "missing flags");
  flags = argv[1] + 1;
  filename = argv[2];
  x = argv[3];
  for (p = x; *p && *p != '\n'; ++p) {}
  xsize = p - x;  /* Make sure x[:psize] doesn't contain '\n'. */
  if (argc == 4) {
    y = NULL;
    ysize = 0;
  } else {
    y = argv[4];
    for (p = y; *p && *p != '\n'; ++p) {}
    ysize = p - y;  /* Make sure x[:psize] doesn't contain '\n'. */
  }
  /* TODO(pts): Make the initial lo and hi offsets specifiable. */
  for (p = flags; (flag = *p); ++p) {
    if (flag == 'e') {
      if (cm != CM_UNSET) usage_error(argv[0], "multiple boundary flags");
      cm = CM_LE;
    } else if (flag == 't') {
      if (cm != CM_UNSET) usage_error(argv[0], "multiple boundary flags");
      cm = CM_LT;
    } else if (flag == 'p') {
      if (cm != CM_UNSET) usage_error(argv[0], "multiple boundary flags");
      cm = CM_LP;
    } else if (flag == 'b') {
      if (cmstart != CM_UNSET) usage_error(argv[0], "multiple start flags");
      cmstart = CM_LE;
    } else if (flag == 'a') {
      if (cmstart != CM_UNSET) usage_error(argv[0], "multiple start flags");
      cmstart = CM_LT;
    } else if (flag == 'o') {
      if (printing != PR_UNSET) usage_error(argv[0], "multiple printing flags");
      printing = PR_OFFSETS;
    } else if (flag == 'c') {
      if (printing != PR_UNSET) usage_error(argv[0], "multiple printing flags");
      printing = PR_CONTENTS;
    } else if (flag == 'q') {
      if (printing != PR_UNSET) usage_error(argv[0], "multiple printing flags");
      printing = PR_DETECT;
    } else if (flag == 'i') {
      if (incomplete != IN_UNSET) {
        usage_error(argv[0], "multiple incomplete flags");
      }
      incomplete = IN_IGNORE;
    } else {
      usage_error(argv[0], "unsupported flag");
    }
  }
  if (printing == PR_UNSET) printing = PR_CONTENTS;
  if (incomplete == IN_UNSET) incomplete = IN_USE;
  if (cmstart == CM_UNSET) cmstart = CM_LE;
  if (cm == CM_UNSET) usage_error(argv[0], "missing boundary flag");
  if (cmstart == CM_LT && !(!y && cm == CM_LE && printing == PR_OFFSETS)) {
    /* TODO(pts): Make cmstart=CM_LT work in bisect_interval etc. */
    usage_error(argv[0], "flag -a needs -eo and no <key-y>");
  }
  if (!y && printing != PR_OFFSETS && cm == CM_LE) {
    usage_error(argv[0], "single-key contents is always empty");
  }

  yfopen(yf, filename, (off_t)-1);
  if (!y && cm == CM_LE && printing == PR_OFFSETS) {
    start = bisect_way(yf, 0, (off_t)-1, x, xsize, cmstart);
    yfclose(yf);
    ofsp = ofsbuf;
    ofsp = format_unsigned(ofsp, start);
    *ofsp++ = '\n';
    write_all_to_stdout(ofsbuf, ofsp - ofsbuf);
  } else if (printing == PR_DETECT &&
             (!y || (xsize == ysize && 0 == memcmp(x, y, xsize)))) {
    /* This branch is just a shortcut, it doesn't change the results. */
    struct cache_entry entry;
    /* Shortcut just to detect if x is present. */
    if (cm == CM_LE) exit(3);  /* start:end range would always be empty. */
    start = bisect_way(yf, 0, (off_t)-1, x, xsize, CM_LE);
    entry.fofs = get_fofs(yf, start);
    entry.ofs = start;
    entry.cmp_result = compare_line(yf, entry.fofs, x, xsize, cm);
    yfclose(yf);
    if (entry.cmp_result) exit(3);  /* exit(3) iff x not found in yf. */
  } else {
    if (!y) {
      y = x;
      ysize = xsize;
    }
    bisect_interval(yf, 0, (off_t)-1, cm, x, xsize, y, ysize, &start, &end);
    if (printing == PR_CONTENTS) {
      print_range(yf, start, end);
    } else if (printing == PR_OFFSETS) {
      ofsp = ofsbuf;
      ofsp = format_unsigned(ofsp, start);
      *ofsp++ = ' ';
      ofsp = format_unsigned(ofsp, end);
      *ofsp++ = '\n';
      write_all_to_stdout(ofsbuf, ofsp - ofsbuf);
    }
    yfclose(yf);
    if (start >= end) exit(3);  /* No match found. */
  }
  return EXIT_SUCCESS;  /* 0. */
}
