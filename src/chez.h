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

#ifndef EMACS_CHEZ_H
#define EMACS_CHEZ_H

#ifdef HAVE_CHEZ

#ifndef EMACS_SCHEME_H_INCLUDED
#define EMACS_SCHEME_H_INCLUDED
/* Chez Scheme exports Scons, Sstring, Smake_string, Smake_vector which
   collide with Emacs DEFUN symbols (which create static variables like
   `static union Aligned_Lisp_Subr Scons = ...`).

   Strategy: rename the Chez functions before including scheme.h so the
   declarations use chez__* names.  Then provide wrapper functions
   (defined in chez.c) that call the real library symbols.  */
#define Scons       chez__Scons
#define Sstring     chez__Sstring
#define Smake_string  chez__Smake_string
#define Smake_vector  chez__Smake_vector
#include <scheme.h>
#undef Scons
#undef Sstring
#undef Smake_string
#undef Smake_vector

/* Wrapper function declarations.  These are defined in chez.c and
   call the real library functions by their original names.  */
extern ptr chez_Scons_real (ptr a, ptr b);
extern ptr chez_Sstring_real (const char *s);
extern ptr chez_Smake_string_real (iptr n, int c);
extern ptr chez_Smake_vector_real (iptr n, ptr v);

/* Provide convenience macros.  Code should use chez_Scons etc.  */
#define chez_Scons(a,b)        chez_Scons_real((a),(b))
#define chez_Sstring(s)        chez_Sstring_real(s)
#define chez_Smake_string(n,c) chez_Smake_string_real((n),(c))
#define chez_Smake_vector(n,v) chez_Smake_vector_real((n),(v))

/* Chez Scheme's `ptr` typedef (void *) collides with Emacs code that
   uses `ptr` as a local variable name (e.g., keyboard.c).  Chez macros
   like Sfixnum(x) expand to ((ptr)(uptr)((x)*8)), which breaks when
   `ptr` is shadowed by a local.

   Fix: redefine all Chez macros that cast via `ptr` to use `void *`
   directly.  We also save the Chez `ptr` type as `chez_ptr` for
   explicit use.  */
typedef ptr chez_ptr;

/* Redefine all Chez macros to use chez_ptr instead of ptr, and
   unsigned long instead of uptr.  Values taken directly from scheme.h
   for tarm64osx.  */
#undef Sfixnum
#define Sfixnum(x) ((chez_ptr)(unsigned long)((x)*8))
#undef Sfixnump
#define Sfixnump(x) (((unsigned long)(x)&0x7)==0x0)
#undef Sfixnum_value
#define Sfixnum_value(x) ((long)(x)/8)
#undef Schar
#define Schar(x) ((chez_ptr)(unsigned long)(((x)<<8)|0x16))
#undef Scharp
#define Scharp(x) (((unsigned long)(x)&0xFF)==0x16)
#undef Schar_value
#define Schar_value(x) ((long)(x)>>8)
#undef Snil
#define Snil ((chez_ptr)0x26)
#undef Strue
#define Strue ((chez_ptr)0xE)
#undef Sfalse
#define Sfalse ((chez_ptr)0x6)
#undef Svoid
#define Svoid ((chez_ptr)0x2E)
#undef Seof_object
#define Seof_object ((chez_ptr)0x36)
#undef Sbwp_object
#define Sbwp_object ((chez_ptr)0x4E)
#undef Snullp
#define Snullp(x) ((unsigned long)(x)==0x26)
#undef Spairp
#define Spairp(x) (((unsigned long)(x)&0x7)==0x1)
#undef Scar
#define Scar(x) (*(chez_ptr *)TO_VOIDP((unsigned long)(x)+7))
#undef Scdr
#define Scdr(x) (*(chez_ptr *)TO_VOIDP((unsigned long)(x)+15))
/* Write operations MUST go through library functions to maintain the
   GC write barrier (dirty card table).  Direct memory stores bypass
   S_dirty_set, causing the generational GC to miss inter-generational
   references during dirty sweep — resulting in stale pointers after
   collection.  These wrapper functions are defined in chez_bridge.c
   which includes scheme.h without our macro overrides.  */
extern void chez_set_car_wb (chez_ptr, chez_ptr);
extern void chez_set_cdr_wb (chez_ptr, chez_ptr);
extern void chez_vector_set_wb (chez_ptr, long, chez_ptr);

/* Sset_car / Sset_cdr go through the write barrier to maintain dirty
   cards for the generational GC.  Direct-store _init variants are
   available for cases where the target is known to be gen-0 (freshly
   allocated), but XSETCAR/XSETCDR MUST use the barrier versions since
   they may modify older-generation objects.  */
#undef Sset_car
#define Sset_car(x,y) chez_set_car_wb((x),(y))
#undef Sset_cdr
#define Sset_cdr(x,y) chez_set_cdr_wb((x),(y))
#define Sset_car_init(x,y) (*(chez_ptr *)TO_VOIDP((unsigned long)(x)+7) = (y))
#define Sset_cdr_init(x,y) (*(chez_ptr *)TO_VOIDP((unsigned long)(x)+15) = (y))

/* Svector macros — Chez vectors have tag 0x7 in low 3 bits */
#undef Svectorp
#define Svectorp(x) (((unsigned long)(x)&0x7)==0x7 && \
    ((unsigned long)(*(chez_ptr *)TO_VOIDP((unsigned long)(x)+1))&0x7)==0x0)
#undef Svector_length
#define Svector_length(x) ((long)((unsigned long)(*(long *)TO_VOIDP((unsigned long)(x)+1)))>>4)
#undef Svector_ref
#define Svector_ref(x,i) (((chez_ptr *)TO_VOIDP((unsigned long)(x)+9))[i])
/* Svector_set goes through the write barrier for inter-generational
   safety.  For initializing freshly allocated gen-0 objects where no
   barrier is needed (the GC always scans gen-0 fully), use the _init
   variant to avoid function-call overhead.  */
#undef Svector_set
#define Svector_set(x,i,v) chez_vector_set_wb((x),(i),(v))
#define Svector_set_init(x,i,v) (((chez_ptr *)TO_VOIDP((unsigned long)(x)+9))[i] = (v))

/* Bytevector macros — tag 0x7 with secondary tag check */
#undef Sbytevectorp
#define Sbytevectorp(x) (((unsigned long)(x)&0x7)==0x7 && \
    ((unsigned long)(*(chez_ptr *)TO_VOIDP((unsigned long)(x)+1))&0x3)==0x1)
#undef Sbytevector_length
#define Sbytevector_length(x) ((long)((unsigned long)(*(long *)TO_VOIDP((unsigned long)(x)+1)))>>3)
#undef Sbytevector_u8_ref
#define Sbytevector_u8_ref(x,i) (((unsigned char *)TO_VOIDP((unsigned long)(x)+9))[i])
#undef Sbytevector_data
#define Sbytevector_data(x) (&Sbytevector_u8_ref(x,0))

/* Flonum — tag 0x2 */
#undef Sflonump
#define Sflonump(x) (((unsigned long)(x)&0x7)==0x2)
#undef Sflonum_value
#define Sflonum_value(x) (*(double *)TO_VOIDP((unsigned long)(x)+6))

/* Bignum — tag 0x7 with secondary tag 0x6 in low 5 bits */
#undef Sbignump
#define Sbignump(x) (((unsigned long)(x)&0x7)==0x7 && \
    ((unsigned long)(*(chez_ptr *)TO_VOIDP((unsigned long)(x)+1))&0x1F)==0x6)

#endif

/* Initialize the Chez Scheme runtime.  Must be called before any
   Lisp_Object allocation.  */
extern void init_chez (void);

/* Register Chez-related Lisp symbols.  */
extern void syms_of_chez (void);

/* Chez type tags for Emacs compound types stored as Chez vectors.
   Slot 0 of each vector holds a fixnum with the tag.  */
enum chez_type_tag
  {
    CHEZ_SYMBOL_TAG    = 0x200,
    CHEZ_STRING_TAG    = 0x201,
    CHEZ_VECTORLIKE_TAG = 0x202,
    CHEZ_PVEC_BASE     = 0x400,
    CHEZ_SUBR_TAG      = 0x411,  /* CHEZ_PVEC_BASE + PVEC_SUBR (17) */
  };

/* Symbol vector layout: [tag|flags, name, value, function, plist, next]  */
#define CHEZ_SYM_SLOTS 6
#define CHEZ_SYM_SLOT_TAG   0
#define CHEZ_SYM_SLOT_NAME  1
#define CHEZ_SYM_SLOT_VAL   2
#define CHEZ_SYM_SLOT_FUNC  3
#define CHEZ_SYM_SLOT_PLIST 4
#define CHEZ_SYM_SLOT_NEXT  5

/* String vector layout: [tag, bytevector, size_byte, intervals]  */
#define CHEZ_STR_SLOTS 4
#define CHEZ_STR_SLOT_TAG       0
#define CHEZ_STR_SLOT_DATA      1
#define CHEZ_STR_SLOT_SIZEBYTE  2
#define CHEZ_STR_SLOT_INTERVALS 3

/* Subr vector layout: [tag, fn_ptr, min_args, max_args, name, intspec]  */
#define CHEZ_SUBR_SLOTS 6

/* Maximum number of DEFSYMs.  */
#define CHEZ_MAX_SYMS 4096

/* Global symbol table (pinned from GC).  */
extern chez_ptr chez_symbols[];
extern int chez_nsyms;

/* Allocate a new Emacs symbol backed by a Chez vector.  */
extern chez_ptr chez_make_symbol (const char *name);

/* Allocate a new Emacs string backed by a Chez vector.  */
extern chez_ptr chez_make_string (const char *data, ptrdiff_t nbytes);

/* Allocate a multibyte string.  */
extern chez_ptr chez_make_multibyte_string (const char *data, ptrdiff_t nchars,
                                       ptrdiff_t nbytes);

/* Allocate a pseudovector with the given type tag and field count.  */
extern chez_ptr chez_make_pseudovector (int pvec_type, int lisp_fields,
                                   int total_slots);

/* Trigger a Chez GC collection at a safe point.  */
extern void chez_maybe_gc (void);

/* Allocation counter — incremented by maybe_gc() in lisp.h.
   Actual GC happens at safe points via chez_gc_safe_point().  */
extern int chez_gc_alloc_count;

/* Nesting depth of readevalloop.  GC only fires at depth <= 1.  */
extern int chez_readevalloop_depth;

/* Check the allocation counter and run GC if threshold exceeded.
   Call from places where the C stack is shallow (readevalloop,
   command_loop) so Chez's copying GC won't invalidate C locals.  */
extern void chez_gc_safe_point (void);

/* GC from within eval_sub / Ffuncall.  Caller must have pinned all
   live C locals holding movable Chez pointers.  */
extern void chez_gc_from_eval (void);

/* Set the C stack base for conservative stack scanning during GC.
   Call once from main() before any Lisp evaluation.  */
extern void chez_set_stack_base (void *base);

/* Register a C-allocated pseudovector so its Lisp_Object fields
   are locked during GC.  Called from chez_wrap_c_pseudovector.  */
extern void chez_register_pseudovector (void *ptr, chez_ptr wrapper);

/* Register a static C struct for GC root scanning (no wrapper needed).
   Used for buffer_defaults, buffer_local_symbols, etc.  */
extern void chez_register_static_roots (void *ptr);

/* Wrapper cache: avoid creating duplicate wrappers for the same
   C struct.  Returns 0 if not cached.  */
extern chez_ptr chez_lookup_wrapper (void *ptr);
extern void chez_cache_wrapper (void *ptr, chez_ptr wrapper);

/* Callback for bytecode stack root locking (set by bytecode.c).  */
extern void (*chez_lock_bytecode_fn) (void (*) (chez_ptr));

/* Global roots vector — holds all global Lisp_Object variables.  */
extern chez_ptr chez_global_roots;

/* Backing symbol vectors for nil and t.
   Snil and Strue are immediates (0x26, 0xE) — not Chez vectors.
   But Emacs treats nil and t as symbols with name/plist/function.
   These backing vectors hold that data.  All symbol accessors
   (SYMBOL_NAME, SYMBOL_PLIST, etc.) must map Snil → chez_nil_symbol
   and Strue → chez_t_symbol (= chez_symbols[iQt]).  */
extern chez_ptr chez_nil_symbol;

/* Resolve a Lisp_Object that passes SYMBOLP to the backing symbol vector.
   For Snil → chez_nil_symbol, for everything else (including Strue/Qt
   which is already a vector) → x itself.  */
INLINE chez_ptr
chez_resolve_symbol (chez_ptr x)
{
  if (Snullp (x))
    return chez_nil_symbol;
  return x;
}

#endif /* HAVE_CHEZ */
#endif /* EMACS_CHEZ_H */
