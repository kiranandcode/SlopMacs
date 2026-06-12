/* Test mmap availability */
#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>

int main(void) {
  for (int i = 0; i < 1000; i++) {
    void *p = mmap(NULL, 2 * 1024 * 1024,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
      fprintf(stderr, "mmap failed at iteration %d: %s\n", i, strerror(errno));
      return 1;
    }
    /* Don't munmap — simulate accumulation */
  }
  fprintf(stderr, "1000 x 2MB mmaps succeeded (2GB total)\n");
  return 0;
}
