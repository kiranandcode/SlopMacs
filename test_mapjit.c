/* Test MAP_JIT allocation on macOS ARM64 */
#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>

int main(void) {
  /* Test regular mmap first */
  for (int i = 0; i < 100; i++) {
    void *p = mmap(NULL, 2 * 1024 * 1024,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
      fprintf(stderr, "Regular mmap failed at %d: %s\n", i, strerror(errno));
      return 1;
    }
  }
  fprintf(stderr, "100 regular mmaps OK (200MB)\n");

  /* Test MAP_JIT */
  for (int i = 0; i < 100; i++) {
    void *p = mmap(NULL, 2 * 1024 * 1024,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) {
      fprintf(stderr, "MAP_JIT mmap failed at %d: %s\n", i, strerror(errno));
      /* Try without JIT */
      void *q = mmap(NULL, 2 * 1024 * 1024,
                     PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (q == MAP_FAILED)
        fprintf(stderr, "  RWX without JIT also failed: %s\n", strerror(errno));
      else
        fprintf(stderr, "  RWX without JIT succeeded\n");
      return 1;
    }
  }
  fprintf(stderr, "100 MAP_JIT mmaps OK (200MB)\n");
  return 0;
}
