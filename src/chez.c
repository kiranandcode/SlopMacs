/* Chez Scheme runtime integration for Emacs.
   Copyright (C) 2026 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

#include <config.h>

#ifdef HAVE_CHEZ

#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <execinfo.h>
#include <errno.h>
#include <time.h>
#include <mach/mach.h>
#include "lisp.h"
#include "thread.h"
#include "chez.h"
#include "charset.h"

/* Declare Chez's memory tracking functions.  */
extern unsigned long S_curmembytes (void);
extern unsigned long S_maxmembytes (void);

/* Get Chez segment generation for a pointer.
   Uses Chez's internal 3-level segment table.
   tarm64osx: segment_offset_bits=14, t1=16, t2=17, t3=17, typemod=8.  */
extern void *S_segment_info[];  /* Actually t2table *[1<<17].  */

/* Get Chez segment info for a pointer.
   Returns generation in low 8 bits, space in bits 8-15.  */
int
chez_ptr_generation (ptr p)
{
  uptr seg = ((uptr) p + 7) >> 14;  /* typemod-1=7, offset=14.  */
  uptr t3 = seg >> 33;              /* t2_bits + t1_bits = 17+16 = 33.  */
  uptr t2 = (seg >> 16) & 0x1FFFF; /* t2_bits = 17, mask = (1<<17)-1.  */
  uptr t1 = seg & 0xFFFF;          /* t1_bits = 16, mask = (1<<16)-1.  */

  void **t2arr = (void **) S_segment_info[t3];
  if (!t2arr)
    return -1;
  void **t1arr = (void **) t2arr[t2];
  if (!t1arr)
    return -1;
  void *si = t1arr[t1];
  if (!si)
    return -1;
  /* seginfo struct: { unsigned char space; unsigned char generation; ... }
     space at offset 0, generation at offset 1.  */
  unsigned char space = ((unsigned char *) si)[0];
  unsigned char gen = ((unsigned char *) si)[1];
  return gen | (space << 8);
}

static int chez_ptr_gen (ptr p) { return chez_ptr_generation (p) & 0xFF; }
static int chez_ptr_space (ptr p) { return (chez_ptr_generation (p) >> 8) & 0xFF; }

static void
chez_abort_handler (int sig)
{
  void *frames[30];
  int n = backtrace (frames, 30);
  fprintf (stderr, "\n=== CHEZ ABORT HANDLER ===\n");
  fprintf (stderr, "Chez membytes: %lu MB (max: %lu MB)\n",
	   S_curmembytes () / (1024UL * 1024UL),
	   S_maxmembytes () / (1024UL * 1024UL));
  fprintf (stderr, "errno: %d (%s)\n", errno, strerror (errno));
  /* Check mach VM regions to understand memory layout.  */
  {
    struct task_basic_info info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info (mach_task_self (), TASK_BASIC_INFO,
		   (task_info_t) &info, &count) == KERN_SUCCESS)
      fprintf (stderr, "RSS: %lu MB  Virtual: %lu MB\n",
	       (unsigned long) info.resident_size / (1024UL * 1024UL),
	       (unsigned long) info.virtual_size / (1024UL * 1024UL));
  }
  fprintf (stderr, "Backtrace:\n");
  backtrace_symbols_fd (frames, n, 2);
  fprintf (stderr, "=========================\n");
  signal (SIGABRT, SIG_DFL);
  raise (SIGABRT);
}

/* Global symbol table.  All entries are pinned via Slock_object
   so the copying GC marks them in-place rather than copying.  */
chez_ptr chez_symbols[CHEZ_MAX_SYMS];
int chez_nsyms = 0;

/* Single pinned vector holding all global Lisp_Object roots.
   Chez GC traces through this to find live objects.  */
chez_ptr chez_global_roots = 0;

/* Backing symbol vector for nil.  Snil (0x26) is an immediate,
   so we need a real symbol vector to hold nil's name, plist, etc.  */
chez_ptr chez_nil_symbol = 0;

/* Forward declaration — defined in pseudovector registry section.  */
static chez_ptr pvec_guardian;

/* Allocation counter — incremented by maybe_gc() in lisp.h.
   Reset to 0 each time chez_gc_safe_point() collects.  */
int chez_gc_alloc_count = 0;

/* Nesting depth of readevalloop.  GC only fires at depth 1
   (the topmost readevalloop), when no eval_sub frames from
   outer readevalloops are on the C stack.  This prevents the
   copying GC from invalidating C locals in caller frames.  */
int chez_readevalloop_depth = 0;

/* Cached Chez procedures — looked up once at init, pinned from GC.
   This avoids allocating (Sstring_to_symbol) during collection.  */
static chez_ptr chez_collect_proc;
static chez_ptr chez_bytes_allocated_proc;

/* Boot file directory, set by configure.  */
#ifndef CHEZ_BOOTDIR
#define CHEZ_BOOTDIR "/usr/local/lib/csv10.1.0/ta6osx"
#endif

static void
chez_suppress_auto_collect (void)
{
  /* CRITICAL: Suppress Chez auto-collection.

     Chez's copying GC moves objects in memory, but Emacs stores
     ptr values in C heap structures (specpdl, handler list,
     pseudovectors) that Chez can't scan.  We must lock all these
     roots before collecting and unlock them after.

     Chez auto-collects via:
       allocation → S_maybe_fire_collector → SOMETHINGPENDING
       → trap handler → collect-request-handler → (collect)

     This fires during ANY Scall* entry point, with roots unlocked.
     Two fixes applied:

     1. Set collect-request-handler to a no-op lambda so even when
        SOMETHINGPENDING fires, no actual GC happens.

     2. Set collect-trip-bytes to ~1PB so queued_fire is rarely set,
        reducing trap overhead.

     Our manual chez_maybe_gc() calls (collect) directly after locking
     roots, bypassing the handler entirely.  */

  /* 1. Set collect-request-handler to (lambda () (void)).
     Build the S-expression as a cons tree and eval it.  */
  chez_ptr eval_proc
    = Stop_level_value (Sstring_to_symbol ("eval"));
  chez_ptr crh_sym = Sstring_to_symbol ("collect-request-handler");
  chez_ptr lambda_sym = Sstring_to_symbol ("lambda");
  chez_ptr void_sym = Sstring_to_symbol ("void");
  /* (void) */
  chez_ptr void_call = chez_Scons (void_sym, Snil);
  /* (lambda () (void)) */
  chez_ptr lambda_form
    = chez_Scons (lambda_sym,
		  chez_Scons (Snil, chez_Scons (void_call, Snil)));
  /* (collect-request-handler (lambda () (void))) */
  chez_ptr expr
    = chez_Scons (crh_sym, chez_Scons (lambda_form, Snil));
  Scall1 (eval_proc, expr);

  /* 2. Set collect-trip-bytes high enough that Chez doesn't auto-
     collect behind our back (we need to lock C-heap roots first).
     16 MB balances allocation bursts vs memory pressure.  Our
     explicit chez_maybe_gc() handles collection at safe points
     with proper root scanning.  */
  chez_ptr ctb_proc
    = Stop_level_value (Sstring_to_symbol ("collect-trip-bytes"));
  Scall1 (ctb_proc, Sfixnum (16L * 1024 * 1024));

  /* 3. Release freed segments back to the OS after every GC cycle.
     By default Chez keeps freed segments in a freelist (never
     returning them via munmap), causing massive RSS even with tiny
     live heaps.  Setting release-minimum-generation to 0 makes
     ALL collections (including gen-0) release segments.  */
  chez_ptr rmg_proc
    = Stop_level_value (Sstring_to_symbol ("release-minimum-generation"));
  Scall1 (rmg_proc, Sfixnum (0));

  /* 4. Minimize heap reserve.  By default Chez keeps free memory
     equal to the live heap (ratio 1.0) for allocation headroom.
     During bootstrap this causes massive RSS.  Ratio 0.5 halves
     the retained free memory.  */
  chez_ptr hrr_sym = Sstring_to_symbol ("heap-reserve-ratio");
  chez_ptr hrr_proc = Stop_level_value (hrr_sym);
  Scall1 (hrr_proc, Sflonum (0.25));
}

static void
chez_cache_gc_procs (void)
{
  /* Cache GC-related procedures so we never allocate during collection.  */
  chez_collect_proc
    = Stop_level_value (Sstring_to_symbol ("collect"));
  Slock_object (chez_collect_proc);

  chez_bytes_allocated_proc
    = Stop_level_value (Sstring_to_symbol ("bytes-allocated"));
  Slock_object (chez_bytes_allocated_proc);
}

void
init_chez (void)
{
  /* Install abort handler to get backtrace on Chez OOM.  */
  signal (SIGABRT, chez_abort_handler);

  /* Initialize the Chez Scheme runtime.  */
  Sscheme_init (NULL);

  /* Register boot files.  */
  {
    char petite_path[4096], scheme_path[4096];
    snprintf (petite_path, sizeof petite_path,
              "%s/petite.boot", CHEZ_BOOTDIR);
    snprintf (scheme_path, sizeof scheme_path,
              "%s/scheme.boot", CHEZ_BOOTDIR);
    Sregister_boot_file (petite_path);
    Sregister_boot_file (scheme_path);
  }

  /* Build the heap from the boot files.  */
  Sbuild_heap (NULL, NULL);

  /* Suppress auto-collection — MUST happen before any Emacs allocation.
     This sets collect-request-handler to a no-op so that Chez never
     auto-collects with our C heap roots unlocked.  */
  chez_suppress_auto_collect ();

  /* Cache GC procedures to avoid allocation during collection.  */
  chez_cache_gc_procs ();

  /* Allocate and pin the global roots vector.
     416 globals in globals.h + room for growth.  */
  chez_global_roots = chez_Smake_vector (512, Snil);
  Slock_object (chez_global_roots);

  /* Create the backing symbol vector for nil.
     Snil (0x26) is a Chez immediate, not a vector.  But Emacs treats
     nil as a symbol with name "nil", empty plist, etc.  This vector
     holds that symbol data; chez_resolve_symbol(Snil) returns it.  */
  chez_nil_symbol = chez_Smake_vector (CHEZ_SYM_SLOTS, Sfalse);
  Svector_set (chez_nil_symbol, CHEZ_SYM_SLOT_TAG, Sfixnum (CHEZ_SYMBOL_TAG));
  Svector_set (chez_nil_symbol, CHEZ_SYM_SLOT_NAME, chez_make_string ("nil", 3));
  Svector_set (chez_nil_symbol, CHEZ_SYM_SLOT_VAL, Snil);
  Svector_set (chez_nil_symbol, CHEZ_SYM_SLOT_FUNC, Snil);
  Svector_set (chez_nil_symbol, CHEZ_SYM_SLOT_PLIST, Snil);
  Svector_set (chez_nil_symbol, CHEZ_SYM_SLOT_NEXT, Sfalse);
  Slock_object (chez_nil_symbol);

  /* Create a guardian to track pseudovector wrapper liveness.  */
  {
    chez_ptr make_guardian
      = Stop_level_value (Sstring_to_symbol ("make-guardian"));
    pvec_guardian = Scall0 (make_guardian);
    Slock_object (pvec_guardian);
  }

  /* Qt (the 't' symbol) will be initialized later via define_symbol
     in lread.c, along with all other DEFSYMs.  At that point,
     chez_symbols[iQt] will be set.  */
  chez_nsyms = 0;
}

chez_ptr
chez_make_symbol (const char *name)
{
  chez_ptr sym = chez_Smake_vector (CHEZ_SYM_SLOTS, Sfalse);

  Svector_set (sym, CHEZ_SYM_SLOT_TAG, Sfixnum (CHEZ_SYMBOL_TAG));
  Svector_set (sym, CHEZ_SYM_SLOT_NAME,
	       chez_make_string (name, strlen (name)));
  Svector_set (sym, CHEZ_SYM_SLOT_VAL, Svoid);
  Svector_set (sym, CHEZ_SYM_SLOT_FUNC, Snil);
  Svector_set (sym, CHEZ_SYM_SLOT_PLIST, Snil);
  Svector_set (sym, CHEZ_SYM_SLOT_NEXT, Sfalse);

  return sym;
}

chez_ptr
chez_make_string (const char *data, ptrdiff_t nbytes)
{
  /* Allocate nbytes+1 for null termination — Emacs C code uses SDATA
     as a C string in many places (fprintf, strcmp, file operations).  */
  chez_ptr bv = Smake_bytevector (nbytes + 1, 0);
  memcpy (Sbytevector_data (bv), data, nbytes);
  Sbytevector_u8_set (bv, nbytes, 0);  /* null terminator */

  chez_ptr str = chez_Smake_vector (CHEZ_STR_SLOTS, Sfalse);
  Svector_set (str, CHEZ_STR_SLOT_TAG, Sfixnum (CHEZ_STRING_TAG));
  Svector_set (str, CHEZ_STR_SLOT_DATA, bv);
  Svector_set (str, CHEZ_STR_SLOT_SIZEBYTE, Sfixnum (nbytes));
  Svector_set (str, CHEZ_STR_SLOT_INTERVALS, Snil);

  return str;
}

chez_ptr
chez_make_multibyte_string (const char *data, ptrdiff_t nchars,
                            ptrdiff_t nbytes)
{
  /* For now, same as make_string.  nchars is stored separately
     when we need it — the bytevector length gives nbytes.  */
  (void) nchars;
  return chez_make_string (data, nbytes);
}

chez_ptr
chez_make_pseudovector (int pvec_type, int lisp_fields, int total_slots)
{
  /* Slot 0 encodes: CHEZ_PVEC_BASE + pvec_type  */
  (void) lisp_fields;
  int tag = CHEZ_PVEC_BASE + pvec_type;
  chez_ptr pv = chez_Smake_vector (total_slots + 1, Sfalse);
  Svector_set (pv, 0, Sfixnum (tag));
  return pv;
}

/* --- Wrapper cache ---
   Cache C-pointer → Chez wrapper mappings to avoid creating duplicate
   wrappers for the same C struct.  Uses open-addressing hash table.  */

#define WRAPPER_CACHE_SIZE 8192  /* must be power of 2 */
#define WRAPPER_CACHE_MASK (WRAPPER_CACHE_SIZE - 1)

struct wrapper_cache_entry {
  void *key;       /* C struct pointer (NULL = empty slot) */
  chez_ptr wrapper; /* Chez 2-slot wrapper vector */
};

static struct wrapper_cache_entry wrapper_cache[WRAPPER_CACHE_SIZE];

chez_ptr
chez_lookup_wrapper (void *ptr)
{
  uintptr_t h = ((uintptr_t) ptr >> 4) & WRAPPER_CACHE_MASK;
  for (int i = 0; i < 16; i++)
    {
      int idx = (h + i) & WRAPPER_CACHE_MASK;
      if (wrapper_cache[idx].key == ptr)
	return wrapper_cache[idx].wrapper;
      if (wrapper_cache[idx].key == NULL)
	return (chez_ptr) 0;
    }
  return (chez_ptr) 0;
}

void
chez_cache_wrapper (void *ptr, chez_ptr wrapper)
{
  uintptr_t h = ((uintptr_t) ptr >> 4) & WRAPPER_CACHE_MASK;
  for (int i = 0; i < 16; i++)
    {
      int idx = (h + i) & WRAPPER_CACHE_MASK;
      if (wrapper_cache[idx].key == NULL || wrapper_cache[idx].key == ptr)
	{
	  wrapper_cache[idx].key = ptr;
	  wrapper_cache[idx].wrapper = wrapper;
	  return;
	}
    }
  /* Cache full in this bucket chain — evict oldest.  */
  int idx = h & WRAPPER_CACHE_MASK;
  wrapper_cache[idx].key = ptr;
  wrapper_cache[idx].wrapper = wrapper;
}

/* --- Pseudovector registry ---
   C-allocated pseudovectors (buffers, hash tables, obarrays, etc.)
   contain Lisp_Object fields that Chez's GC can't see.  We track
   all live ones so we can lock their fields before collecting.

   A Chez guardian monitors wrapper vector liveness.  After each GC,
   wrappers that became unreachable are returned by the guardian,
   and we remove them from the registry.  This prevents memory leaks
   from dead pseudovectors keeping their fields alive.  */

/* Callback to lock/unlock bytecode stack roots.  Set by bytecode.c
   where struct bc_frame is visible.  */
void (*chez_lock_bytecode_fn) (void (*) (Lisp_Object));

#define PVEC_REGISTRY_INITIAL 4096

static void **pvec_registry;
static int pvec_registry_count;
static int pvec_registry_capacity;
static chez_ptr pvec_guardian;  /* Chez guardian for tracking liveness */

/* Remove dead entries from the registry by draining the guardian.
   The guardian returns wrapper vectors that became unreachable since
   the last GC.  We extract the C pointer from slot 1 and remove
   the corresponding registry entry.  */
static void
chez_drain_guardian (void)
{
  if (!pvec_guardian)
    return;
  for (;;)
    {
      chez_ptr dead = Scall0 (pvec_guardian);
      if (dead == Sfalse)
	break;
      /* Extract the C struct pointer from the dead wrapper.  */
      void *dead_ptr = (void *)(iptr) Sfixnum_value (Svector_ref (dead, 1));
      for (int i = 0; i < pvec_registry_count; i++)
	{
	  if (pvec_registry[i] == dead_ptr)
	    {
	      pvec_registry[i] = pvec_registry[--pvec_registry_count];
	      break;
	    }
	}
    }
}

static void
pvec_registry_add (void *ptr)
{
  if (!pvec_registry)
    {
      pvec_registry_capacity = PVEC_REGISTRY_INITIAL;
      pvec_registry = malloc (pvec_registry_capacity * sizeof (void *));
    }
  if (pvec_registry_count >= pvec_registry_capacity)
    {
      pvec_registry_capacity *= 2;
      pvec_registry = realloc (pvec_registry,
			       pvec_registry_capacity * sizeof (void *));
    }
  pvec_registry[pvec_registry_count++] = ptr;
}

void
chez_register_pseudovector (void *ptr, chez_ptr wrapper)
{
  pvec_registry_add (ptr);
  /* Register wrapper with guardian for liveness tracking.
     Don't lock the wrapper — the guardian needs it to be collectible.  */
  if (pvec_guardian)
    Scall1 (pvec_guardian, wrapper);
}

/* Register a static C struct (like buffer_defaults) for GC root
   scanning.  Unlike chez_register_pseudovector, this doesn't create
   a wrapper or register with the guardian — static structs are never
   freed and don't need Lisp_Object wrappers.  */
void
chez_register_static_roots (void *ptr)
{
  pvec_registry_add (ptr);
}

/* Lock/unlock a single Lisp_Object for GC safety.
   Slock_object is a no-op for immediates (fixnums, nil, etc.).  */
static void
chez_lock (Lisp_Object x)
{
  Slock_object (x);
}

static void
chez_unlock (Lisp_Object x)
{
  Sunlock_object (x);
}

/* Visit all Lisp_Object fields in a C-allocated pseudovector.
   The header encodes the number of Lisp fields in the low 12 bits
   (PSEUDOVECTOR_SIZE_MASK).  Fields start right after the header.  */
static void
chez_visit_pseudovector (void *ptr, void (*visit) (Lisp_Object))
{
  union vectorlike_header *h = (union vectorlike_header *) ptr;
  if (!(h->size & PSEUDOVECTOR_FLAG))
    return;
  int n_lisp = h->size & PSEUDOVECTOR_SIZE_MASK;
  Lisp_Object *fields
    = (Lisp_Object *) ((char *) ptr + sizeof (union vectorlike_header));
  for (int i = 0; i < n_lisp; i++)
    {
      Lisp_Object val = fields[i];
      /* Skip NULL/zero values (C zero-init before proper Lisp init).  */
      if (val != (Lisp_Object) 0)
	visit (val);
    }
}

/* Walk all Lisp_Object values stored in C heap structures and
   lock/unlock them so Chez's copying GC doesn't move them.  */
static void
chez_visit_c_roots (void (*visit) (Lisp_Object))
{
  /* 1. staticvec — global/static Lisp_Object variables registered
     via staticpro().  */
  for (int i = 0; i < staticidx; i++)
    {
      Lisp_Object val = *staticvec[i];
      if (val != (Lisp_Object) 0)
	visit (val);
    }

  /* 2. specpdl — the binding stack (xmalloc'd memory).  */
  for (union specbinding *pdl = specpdl; pdl != specpdl_ptr; pdl++)
    {
      switch (pdl->kind)
	{
	case SPECPDL_UNWIND:
	  visit (pdl->unwind.arg);
	  break;

	case SPECPDL_UNWIND_ARRAY:
	  for (ptrdiff_t i = 0; i < pdl->unwind_array.nelts; i++)
	    visit (pdl->unwind_array.array[i]);
	  break;

	case SPECPDL_UNWIND_EXCURSION:
	  visit (pdl->unwind_excursion.marker);
	  visit (pdl->unwind_excursion.window);
	  break;

	case SPECPDL_BACKTRACE:
	  visit (pdl->bt.function);
	  {
	    ptrdiff_t nargs = pdl->bt.nargs;
	    if (nargs == UNEVALLED)
	      nargs = 1;
	    if (pdl->bt.args)
	      for (ptrdiff_t i = 0; i < nargs; i++)
		visit (pdl->bt.args[i]);
	  }
	  break;

	case SPECPDL_LET_DEFAULT:
	case SPECPDL_LET_LOCAL:
	  visit (pdl->let.where.buf);
	  FALLTHROUGH;
	case SPECPDL_LET:
	  visit (pdl->let.symbol);
	  visit (pdl->let.old_value);
	  break;

	default:
	  break;
	}
    }

  /* 3. Handler list — catch/condition-case entries.  */
  for (struct handler *h = handlerlist; h; h = h->next)
    {
      visit (h->tag_or_ch);
      visit (h->val);
    }

  /* 4. Bytecode VM stack — handled via chez_lock_bytecode_roots
     (defined in bytecode.c where struct bc_frame is visible).  */
  if (chez_lock_bytecode_fn)
    chez_lock_bytecode_fn (visit);

  /* 5. All registered C pseudovectors — their Lisp_Object fields
     are in xmalloc'd memory invisible to Chez's GC.  Dead entries
     are cleaned up by the guardian before each GC cycle.  */
  for (int i = 0; i < pvec_registry_count; i++)
    {
      chez_visit_pseudovector (pvec_registry[i], visit);

      /* 5b. Type-specific: separately-allocated arrays of Lisp_Object.  */
      union vectorlike_header *hdr = pvec_registry[i];
      int pvtype = (hdr->size >> PSEUDOVECTOR_AREA_BITS) & 0x1F;

      /* Hash table key_and_value is now backed by a locked Chez
	 vector (kv_chez), so the GC traces it automatically.
	 No manual scanning needed.  */
      if (pvtype == PVEC_OBARRAY)
	{
	  struct Lisp_Obarray *oa = pvec_registry[i];
	  if (oa->buckets)
	    {
	      ptrdiff_t sz = obarray_size (oa);
	      for (ptrdiff_t j = 0; j < sz; j++)
		{
		  Lisp_Object v = oa->buckets[j];
		  if (v != (Lisp_Object) 0 && !FIXNUMP (v))
		    visit (v);
		}
	    }
	}
    }
}

/* Fast root visitor: scans staticvec, specpdl, handler chain, and
   pseudovector header fields, but SKIPS hash table key_and_value
   arrays and obarray bucket arrays.  Those are the main cost center
   (potentially 100K+ entries).  Objects in those arrays are mostly
   reachable via other paths (symbols in staticvec, values referenced
   from Lisp code on the specpdl, etc.).  */
static void
chez_visit_c_roots_fast (void (*visit) (Lisp_Object))
{
  /* 1. staticvec — global Lisp_Object variables.  */
  for (int i = 0; i < staticidx; i++)
    {
      Lisp_Object val = *staticvec[i];
      if (val != (Lisp_Object) 0)
	visit (val);
    }

  /* 2. specpdl — binding stack.  */
  for (union specbinding *pdl = specpdl; pdl != specpdl_ptr; pdl++)
    {
      switch (pdl->kind)
	{
	case SPECPDL_UNWIND:
	  visit (pdl->unwind.arg);
	  break;
	case SPECPDL_UNWIND_ARRAY:
	  for (ptrdiff_t i = 0; i < pdl->unwind_array.nelts; i++)
	    visit (pdl->unwind_array.array[i]);
	  break;
	case SPECPDL_UNWIND_EXCURSION:
	  visit (pdl->unwind_excursion.marker);
	  visit (pdl->unwind_excursion.window);
	  break;
	case SPECPDL_BACKTRACE:
	  visit (pdl->bt.function);
	  {
	    ptrdiff_t nargs = pdl->bt.nargs;
	    if (nargs == UNEVALLED)
	      nargs = 1;
	    if (pdl->bt.args)
	      for (ptrdiff_t i = 0; i < nargs; i++)
		visit (pdl->bt.args[i]);
	  }
	  break;
	case SPECPDL_LET_DEFAULT:
	case SPECPDL_LET_LOCAL:
	  visit (pdl->let.where.buf);
	  FALLTHROUGH;
	case SPECPDL_LET:
	  visit (pdl->let.symbol);
	  visit (pdl->let.old_value);
	  break;
	default:
	  break;
	}
    }

  /* 3. Handler chain.  */
  for (struct handler *h = handlerlist; h; h = h->next)
    {
      visit (h->tag_or_ch);
      visit (h->val);
    }

  /* 4. Bytecode VM stack.  */
  if (chez_lock_bytecode_fn)
    chez_lock_bytecode_fn (visit);

  /* 5. Skip pseudovector scanning entirely for minor GC.
     With 65K+ registered pseudovectors (each with ~25 Lisp fields),
     scanning produces 1.6M Slock/Sunlock calls — too slow.
     Objects in pseudovector fields are mostly reachable through
     staticvec globals or the specpdl.  The conservative stack scan
     catches any remaining local references.  */
}

/* Count distinct objects being locked — for diagnostics.  */
static int chez_lock_count;

static void
chez_lock_counted (Lisp_Object x)
{
  /* Skip immediates — Slock_object is no-op for them anyway,
     but counting them pollutes the lock_count diagnostic.  */
  if (Sfixnump (x) || Snullp (x) || x == Strue || x == Sfalse
      || x == Svoid || Scharp (x))
    return;
  Slock_object (x);
  chez_lock_count++;
}

static void
chez_unlock_counted (Lisp_Object x)
{
  if (Sfixnump (x) || Snullp (x) || x == Strue || x == Sfalse
      || x == Svoid || Scharp (x))
    return;
  Sunlock_object (x);
}

/* Conservative stack scanning for GC safety.
   Before calling Chez (collect), we scan the C stack for words that
   look like Chez heap pointers and lock them.  This prevents the
   copying GC from collecting or moving objects still live on the stack.
   After collection, we unlock them.  False positives just prevent
   collection of some garbage — safe, just slightly less effective.  */

#include <setjmp.h>

/* Stack base, set once in main().  */
static void *chez_c_stack_base;

void
chez_set_stack_base (void *base)
{
  chez_c_stack_base = base;
}

/* Check if a word looks like a Chez heap pointer.
   Valid Chez tagged pointers have specific low-bit patterns and
   point into reasonable address ranges.  */
static bool
looks_like_chez_ptr (uintptr_t w)
{
  /* Must be in a plausible heap range */
  if (w < 0x10000 || w > (uintptr_t)0x7FFFFFFFFFFF)
    return false;

  int tag = w & 0x7;
  /* Chez tagged types: pair=1, flonum=2, symbol=3, closure=5,
     immediate=6 (skip — chars/bools), typed_object=7 */
  return (tag == 1 || tag == 2 || tag == 3 || tag == 5 || tag == 7);
}

/* Pin limit — generous to avoid missing stack roots.  */
#define GC_STACK_PINS_MAX 65536

/* Scan a memory range and pin Chez-looking pointers.  */
static int
scan_range_and_pin (uintptr_t lo, uintptr_t hi,
		    chez_ptr *pinned, int npinned)
{
  lo = (lo + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
  for (uintptr_t addr = lo; addr < hi && npinned < GC_STACK_PINS_MAX;
       addr += sizeof(void*))
    {
      uintptr_t w = *(uintptr_t *) addr;
      if (looks_like_chez_ptr (w))
	{
	  chez_ptr p = (chez_ptr) w;
	  Slock_object (p);
	  pinned[npinned++] = p;
	}
    }
  return npinned;
}

/* Count C roots that need vector slots.  */
static int
chez_count_c_roots (void)
{
  int n = 0;

  /* staticvec.  */
  for (int i = 0; i < staticidx; i++)
    {
      Lisp_Object val = *staticvec[i];
      if (val != (Lisp_Object) 0)
	n++;
    }

  /* specpdl.  */
  for (union specbinding *pdl = specpdl; pdl != specpdl_ptr; pdl++)
    {
      switch (pdl->kind)
	{
	case SPECPDL_UNWIND: n++; break;
	case SPECPDL_UNWIND_ARRAY: n += pdl->unwind_array.nelts; break;
	case SPECPDL_UNWIND_EXCURSION: n += 2; break;
	case SPECPDL_BACKTRACE:
	  n++;  /* function */
	  { ptrdiff_t na = pdl->bt.nargs;
	    if (na == UNEVALLED) na = 1;
	    if (pdl->bt.args) n += na;
	  }
	  break;
	case SPECPDL_LET_DEFAULT:
	case SPECPDL_LET_LOCAL: n++; FALLTHROUGH;
	case SPECPDL_LET: n += 2; break;
	default: break;
	}
    }

  /* handler chain.  */
  for (struct handler *h = handlerlist; h; h = h->next)
    n += 2;

  /* pseudovectors — header Lisp fields only (skip ht/obarray arrays).  */
  for (int i = 0; i < pvec_registry_count; i++)
    {
      union vectorlike_header *hdr = pvec_registry[i];
      if (hdr->size & PSEUDOVECTOR_FLAG)
	n += hdr->size & PSEUDOVECTOR_SIZE_MASK;
    }

  /* wrapper cache — Chez wrapper vectors that can move during GC.  */
  for (int i = 0; i < WRAPPER_CACHE_SIZE; i++)
    if (wrapper_cache[i].key != NULL)
      n++;

  return n;
}

/* Pair: source location and slot index in the root vector.
   Used to copy roots into the vector before GC and copy back after.  */
struct root_ref {
  Lisp_Object *source;   /* C memory location holding the Lisp_Object.  */
  int slot;              /* Index in the Chez root vector.  */
};

/* Static buffer for root references.  Grown as needed.  */
static struct root_ref *root_refs;
static int root_refs_size;
static int root_refs_count;

/* Check whether a Lisp_Object is immovable — either pinned via
   Slock_object or a Chez immediate.  Immovable values must NOT be
   placed in the root vector (they don't need GC updating).  */
static bool
chez_immovable_p (Lisp_Object val)
{
  return (SYMBOLP (val) || Sfixnump (val) || Snullp (val)
	  || val == Sfalse || val == Strue || val == Svoid
	  || Scharp (val));
}

static void
root_ref_add (Lisp_Object *src, int slot)
{
  if (root_refs_count >= root_refs_size)
    return;  /* Safety: don't overflow.  */
  root_refs[root_refs_count].source = src;
  root_refs[root_refs_count].slot = slot;
  root_refs_count++;
}

/* Add a C root to the root vector for GC tracing and copy-back.
   Immovable values (pinned symbols, fixnums, nil, booleans, void,
   chars) are excluded — they don't move and don't need updating.
   Returns true if the value was added, false if skipped.  */
static bool
root_add (chez_ptr root_vec, Lisp_Object *src, int *slotp)
{
  Lisp_Object val = *src;
  if (val == (Lisp_Object) 0 || chez_immovable_p (val))
    return false;

  /* Safety: validate that tagged pointers are valid Chez objects.
     Uninitialized C memory or corrupt pointers would cause the GC
     to chase invalid memory during copy.  */
  uptr tag = (uptr) val & 0x7;
  if (tag != 0)
    {
      uptr addr = (uptr) val & ~(uptr) 0x7;
      if (addr < 0x100000 || addr > 0x1000000000000ULL)
	return false;
    }
  /* For typed objects (tag 7), verify the header is sane.  */
  if (tag == 0x7)
    {
      uptr hdr = *(uptr *) TO_VOIDP ((uptr) val + 1);
      if ((hdr >> 3) > 64 * 1024 * 1024)
	return false;
    }

  Svector_set (root_vec, *slotp, val);
  root_ref_add (src, *slotp);
  (*slotp)++;
  return true;
}

/* Persistent root vector, registered with S_protect so Chez's GC
   natively traces and updates its contents.  Unlike Slock_object
   (which pins an object but may not trace its slots during gen-0
   collection), S_protect guarantees the GC processes the value
   as a direct root — tracing all reachable objects and updating
   pointers after copying.  */
static chez_ptr persistent_root_vec = 0;
static bool root_vec_protected = false;

void
chez_maybe_gc (void)
{
  static bool in_gc = false;

  if (in_gc)
    return;

  if (!chez_collect_proc || !chez_c_stack_base)
    return;

  in_gc = true;

  /* 1. Count how many root slots we need.  */
  int nroots = chez_count_c_roots ();

  /* 2. Allocate/grow persistent root vector.  Registered with
     S_protect once, so the GC always traces it as a direct root.  */
  int needed = nroots + 16;
  if (!root_vec_protected)
    {
      extern void S_protect (ptr *p);
      S_protect ((ptr *) &persistent_root_vec);
      root_vec_protected = true;
    }
  if (!Svectorp (persistent_root_vec)
      || Svector_length (persistent_root_vec) < needed)
    persistent_root_vec = chez_Smake_vector (needed, Sfalse);
  else
    {
      /* Clear previous root entries.  */
      int len = Svector_length (persistent_root_vec);
      for (int i = 0; i < needed && i < len; i++)
	Svector_set (persistent_root_vec, i, Sfalse);
    }
  chez_ptr root_vec = persistent_root_vec;

  if (root_refs_size < nroots + 16)
    {
      xfree (root_refs);
      root_refs_size = nroots + 256;
      root_refs = xmalloc (root_refs_size * sizeof *root_refs);
    }
  root_refs_count = 0;
  int slot = 0;

  /* 3. Copy movable C roots into the vector.  Immovable values
     (pinned symbols, fixnums, etc.) are excluded — they don't
     move during GC so their C pointers remain valid.  */

  /* 3a. staticvec.  */
  for (int i = 0; i < staticidx; i++)
    root_add (root_vec, staticvec[i], &slot);

  /* 3b. specpdl.  */
  for (union specbinding *pdl = specpdl; pdl != specpdl_ptr; pdl++)
    {
      switch (pdl->kind)
	{
	case SPECPDL_UNWIND:
	  root_add (root_vec, &pdl->unwind.arg, &slot);
	  break;

	case SPECPDL_UNWIND_ARRAY:
	  for (ptrdiff_t i = 0; i < pdl->unwind_array.nelts; i++)
	    root_add (root_vec, &pdl->unwind_array.array[i], &slot);
	  break;

	case SPECPDL_UNWIND_EXCURSION:
	  root_add (root_vec, &pdl->unwind_excursion.marker, &slot);
	  root_add (root_vec, &pdl->unwind_excursion.window, &slot);
	  break;

	case SPECPDL_BACKTRACE:
	  root_add (root_vec, &pdl->bt.function, &slot);
	  {
	    ptrdiff_t na = pdl->bt.nargs;
	    if (na == UNEVALLED) na = 1;
	    if (pdl->bt.args)
	      for (ptrdiff_t i = 0; i < na; i++)
		root_add (root_vec, &pdl->bt.args[i], &slot);
	  }
	  break;

	case SPECPDL_LET_DEFAULT:
	case SPECPDL_LET_LOCAL:
	  root_add (root_vec, &pdl->let.where.buf, &slot);
	  FALLTHROUGH;
	case SPECPDL_LET:
	  root_add (root_vec, &pdl->let.symbol, &slot);
	  root_add (root_vec, &pdl->let.old_value, &slot);
	  break;

	default:
	  break;
	}
    }

  /* 3c. Handler chain.  */
  for (struct handler *h = handlerlist; h; h = h->next)
    {
      root_add (root_vec, &h->tag_or_ch, &slot);
      root_add (root_vec, &h->val, &slot);
    }

  /* 3d. Bytecode VM stack.  */
  if (chez_lock_bytecode_fn)
    chez_lock_bytecode_fn (chez_lock);

  /* 3e. Pseudovector Lisp_Object fields.
     Scan the header-embedded Lisp fields (name, plist, etc.)
     but skip separately-allocated hash table key_and_value arrays
     and obarray bucket arrays — those are the O(100K+) cost centers
     and their contents (symbols=pinned, values=referenced from Lisp
     code on the specpdl) are reachable through other roots.  */
  for (int i = 0; i < pvec_registry_count; i++)
    {
      union vectorlike_header *hdr = pvec_registry[i];
      if (!(hdr->size & PSEUDOVECTOR_FLAG))
	continue;
      int n_lisp = hdr->size & PSEUDOVECTOR_SIZE_MASK;
      Lisp_Object *fields
	= (Lisp_Object *) ((char *) hdr + sizeof (union vectorlike_header));
      for (int j = 0; j < n_lisp; j++)
	root_add (root_vec, &fields[j], &slot);
    }

  /* 3f. Wrapper cache — Chez wrapper vectors stored in C memory.  */
  for (int i = 0; i < WRAPPER_CACHE_SIZE; i++)
    {
      if (wrapper_cache[i].key != NULL)
	root_add (root_vec, &wrapper_cache[i].wrapper, &slot);
    }

  /* 4. Drain dead pseudovectors from the guardian.  */
  chez_drain_guardian ();

  /* 5. Collect.  Alternate gen-0 and gen-4 (full) collections.
     Gen-0 is cheap (only copies nursery objects).  Gen-4 promotes
     long-lived objects and releases all segments back to the OS.
     Alternating ensures gen-1 segments never accumulate for more
     than one cycle.  */
  {
    static int gen4_countdown = 2;
    static int gc_cycle = 0;
    int gen = 0;
    if (--gen4_countdown <= 0)
      {
	gen = 4;
	gen4_countdown = 2;
      }

    struct timespec gc_t0, gc_t1;
    clock_gettime (CLOCK_MONOTONIC, &gc_t0);

    Scall1 (chez_collect_proc, Sfixnum (gen));

    clock_gettime (CLOCK_MONOTONIC, &gc_t1);
    long gc_ms = (gc_t1.tv_sec - gc_t0.tv_sec) * 1000
      + (gc_t1.tv_nsec - gc_t0.tv_nsec) / 1000000;
    gc_cycle++;

    /* Query Chez bytes-allocated for memory diagnostics.  */
    long long bytes_after = 0;
    if (chez_bytes_allocated_proc)
      bytes_after = Sfixnum_value (Scall0 (chez_bytes_allocated_proc));

    /* Get RSS from Mach task_info.  */
    long long rss_mb = 0;
    {
      struct mach_task_basic_info info;
      mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
      if (task_info (mach_task_self (), MACH_TASK_BASIC_INFO,
		     (task_info_t) &info, &count) == KERN_SUCCESS)
	rss_mb = info.resident_size / (1024 * 1024);
    }

    if (gc_ms > 10 || gc_cycle % 50 == 0 || gc_cycle <= 20
	|| rss_mb > 500)
      fprintf (stderr,
	       "  [GC#%d gen=%d %ldms roots=%d heap=%lldMB rss=%lldMB]\n",
	       gc_cycle, gen, gc_ms, slot,
	       bytes_after / (1024 * 1024), rss_mb);
  }

  /* The GC may have moved the root vector itself (it's gen-0, not
     locked).  Re-read from the S_protect'd C variable.  */
  root_vec = persistent_root_vec;

  /* 6. Copy updated references back to C memory.  */
  for (int i = 0; i < root_refs_count; i++)
    *root_refs[i].source = Svector_ref (root_vec, root_refs[i].slot);

  /* 7. Unlock bytecode roots.  */
  if (chez_lock_bytecode_fn)
    chez_lock_bytecode_fn (chez_unlock);

  in_gc = false;
}

void
chez_gc_safe_point (void)
{
  if (chez_gc_alloc_count < 50)
    return;

  chez_gc_alloc_count = 0;
  chez_maybe_gc ();
}

/* GC triggered from within eval_sub / Ffuncall.
   The caller must have pinned all C locals that hold movable Chez
   pointers (via chez_gc_pin / Slock_object).  The evaluator already
   does this for locals across recursive eval_sub calls.  */
void
chez_gc_from_eval (void)
{
  chez_gc_alloc_count = 0;
  chez_maybe_gc ();
}

void
syms_of_chez (void)
{
  /* Will register Chez-specific Lisp primitives here.  */
}

#endif /* HAVE_CHEZ */
