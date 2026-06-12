/* Test: simulate Emacs GC pattern — allocate many objects,
   then lock a subset, then collect.  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <scheme.h>

int main(void) {
  Sscheme_init(NULL);

  char petite[256], scheme_path[256];
  snprintf(petite, sizeof petite, "%s/petite.boot",
           "/Users/kirancodes/chez-install/lib/csv10.5.0/tarm64osx");
  snprintf(scheme_path, sizeof scheme_path, "%s/scheme.boot",
           "/Users/kirancodes/chez-install/lib/csv10.5.0/tarm64osx");
  Sregister_boot_file(petite);
  Sregister_boot_file(scheme_path);
  Sbuild_heap(NULL, NULL);

  /* CRITICAL: Disable auto-collect.  Slock_object allocates cons cells
     internally, which can trigger auto-collection while we're in the
     middle of locking roots.  Not-yet-locked objects get moved → stale ptrs.  */
  ptr ctb = Stop_level_value(Sstring_to_symbol("collect-trip-bytes"));
  Scall1(ctb, Sfixnum(1024L * 1024L * 512L));

  ptr bytes_alloc = Stop_level_value(Sstring_to_symbol("bytes-allocated"));
  ptr collect_proc = Stop_level_value(Sstring_to_symbol("collect"));
  Slock_object(bytes_alloc);
  Slock_object(collect_proc);

  fprintf(stderr, "Phase 1: Allocate 50K objects (simulating loadup)...\n");
  /* Simulate what Emacs does during loadup:
     - Intern symbols (6-slot vectors)
     - Make strings (4-slot vectors + bytevectors)
     - Make pairs (cons cells)
     - Make pseudovectors (wrapped C structs as 2-slot vectors)
  */
  #define N_SYMS 2500
  #define N_STRS 5000
  #define N_PAIRS 20000
  #define N_PVECS 2000
  #define N_ROOTS 3000  /* how many to lock */

  ptr *syms = malloc(N_SYMS * sizeof(ptr));
  ptr *strs = malloc(N_STRS * sizeof(ptr));
  ptr *pairs = malloc(N_PAIRS * sizeof(ptr));
  ptr *pvecs = malloc(N_PVECS * sizeof(ptr));

  for (int i = 0; i < N_SYMS; i++) {
    syms[i] = Smake_vector(6, Sfalse);
    Svector_set(syms[i], 0, Sfixnum(0x200));
    char name[32]; snprintf(name, sizeof name, "sym-%d", i);
    ptr bv = Smake_bytevector(strlen(name)+1, 0);
    memcpy(Sbytevector_data(bv), name, strlen(name)+1);
    ptr str = Smake_vector(4, Sfalse);
    Svector_set(str, 0, Sfixnum(0x201));
    Svector_set(str, 1, bv);
    Svector_set(syms[i], 1, str);
    /* Pin symbols like chez_symbols[] */
    Slock_object(syms[i]);
  }

  for (int i = 0; i < N_STRS; i++) {
    ptr bv = Smake_bytevector(64, 0);
    strs[i] = Smake_vector(4, Sfalse);
    Svector_set(strs[i], 0, Sfixnum(0x201));
    Svector_set(strs[i], 1, bv);
  }

  for (int i = 0; i < N_PAIRS; i++)
    pairs[i] = Scons(Sfixnum(i), Snil);

  for (int i = 0; i < N_PVECS; i++) {
    pvecs[i] = Smake_vector(2, Sfalse);
    Svector_set(pvecs[i], 0, Sfixnum(0x400));
    Svector_set(pvecs[i], 1, Sfixnum((iptr)malloc(64))); /* fake C ptr */
  }

  long alloc = Sfixnum_value(Scall0(bytes_alloc));
  fprintf(stderr, "  Allocated: %ld MB\n", alloc / (1024*1024));

  /* Now simulate GC: lock roots, collect, unlock */
  fprintf(stderr, "Phase 2: Lock %d roots...\n", N_ROOTS);

  /* Lock a subset: some strings, some pairs, some pvecs */
  int locked = 0;
  for (int i = 0; i < N_STRS && locked < N_ROOTS; i++, locked++)
    Slock_object(strs[i]);
  for (int i = 0; i < N_PAIRS && locked < N_ROOTS; i++, locked++)
    Slock_object(pairs[i]);
  for (int i = 0; i < N_PVECS && locked < N_ROOTS; i++, locked++)
    Slock_object(pvecs[i]);

  fprintf(stderr, "  Locked %d objects.\n", locked);

  fprintf(stderr, "Phase 3: Collect gen 0...\n");
  Scall1(collect_proc, Sfixnum(0));
  fprintf(stderr, "  Gen 0 done.\n");

  long after = Sfixnum_value(Scall0(bytes_alloc));
  fprintf(stderr, "  After: %ld MB (freed %ld MB)\n",
          after/(1024*1024), (alloc-after)/(1024*1024));

  /* Unlock */
  locked = 0;
  for (int i = 0; i < N_STRS && locked < N_ROOTS; i++, locked++)
    Sunlock_object(strs[i]);
  for (int i = 0; i < N_PAIRS && locked < N_ROOTS; i++, locked++)
    Sunlock_object(pairs[i]);
  for (int i = 0; i < N_PVECS && locked < N_ROOTS; i++, locked++)
    Sunlock_object(pvecs[i]);

  fprintf(stderr, "Phase 4: Full collection...\n");
  Scall0(collect_proc);
  long final = Sfixnum_value(Scall0(bytes_alloc));
  fprintf(stderr, "  After full: %ld MB\n", final/(1024*1024));

  /* Repeat: simulate multiple GC cycles */
  fprintf(stderr, "Phase 5: 10 lock-collect-unlock cycles...\n");
  for (int cycle = 0; cycle < 10; cycle++) {
    /* Allocate some garbage */
    for (int i = 0; i < 5000; i++)
      (void)Scons(Sfixnum(i), Snil);

    /* Lock roots */
    for (int i = 0; i < N_STRS; i++) Slock_object(strs[i]);
    for (int i = 0; i < N_PVECS; i++) Slock_object(pvecs[i]);

    Scall1(collect_proc, Sfixnum(0));

    for (int i = 0; i < N_STRS; i++) Sunlock_object(strs[i]);
    for (int i = 0; i < N_PVECS; i++) Sunlock_object(pvecs[i]);

    alloc = Sfixnum_value(Scall0(bytes_alloc));
    fprintf(stderr, "  Cycle %d: %ld MB\n", cycle, alloc/(1024*1024));
  }

  fprintf(stderr, "Success!\n");
  free(syms); free(strs); free(pairs); free(pvecs);
  Sscheme_deinit();
  return 0;
}
