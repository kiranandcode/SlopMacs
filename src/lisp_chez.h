/* Chez Scheme backed Lisp_Object type system for GNU Emacs.
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

/* This file is included by lisp.h when HAVE_CHEZ is defined.
   It replaces the traditional Emacs tagged-pointer Lisp_Object with
   Chez Scheme's ptr type.  All Emacs types are represented using
   Chez's native types (fixnum, flonum, pair) or as tagged Chez vectors
   (for symbols, strings, pseudovectors, subrs).  */

#ifndef EMACS_LISP_CHEZ_H
#define EMACS_LISP_CHEZ_H

/* chez.h includes <scheme.h> — do not include it again here,
   as scheme.h lacks include guards and has static inline functions.  */
#include "chez.h"
#include <stdlib.h>  /* for abort() */

/* ====================================================================
   Core type definition
   ==================================================================== */

typedef chez_ptr Lisp_Object;

/* Chez doesn't use the Emacs tagged-pointer scheme.  */
typedef chez_ptr Lisp_Word;
#define LISP_WORDS_ARE_POINTERS 1

/* Suppress CHECK_LISP_OBJECT_TYPE — Chez ptr is the type.  */
enum CHECK_LISP_OBJECT_TYPE { CHECK_LISP_OBJECT_TYPE = false };

#define LISP_INITIALLY(w) (w)

/* Conversion macros — identity for Chez.  */
#define lisp_h_XLI(o) ((EMACS_INT) (intptr_t) (o))
#define lisp_h_XIL(i) ((Lisp_Object) (intptr_t) (i))
#define lisp_h_XLP(o) ((void *) (o))
#define XLI(o) lisp_h_XLI (o)
#define XIL(i) lisp_h_XIL (i)
#define XLP(o) lisp_h_XLP (o)

/* ====================================================================
   Qnil & Qt
   ==================================================================== */

/* Emacs nil is Chez's empty list.  */
#define Qnil    Snil

/* Qt is defined in globals.h via builtin_lisp_symbol(iQt),
   which expands to chez_symbols[iQt].  It is initialized at startup
   via init_chez().  Do not redefine Qt here.  */

/* ====================================================================
   Type predicates — lisp_h_* macros
   ==================================================================== */

/* All predicate macros use statement expressions ({ ... }) to evaluate
   their argument exactly once.  This prevents double-evaluation bugs
   when the argument has side effects (e.g., POP, assignments).  */

/* Helper: check if x is a Chez vector with a given tag in slot 0.  */
#define CHEZ_TAGGED_VECTOR_P(x, tag) \
  ({ Lisp_Object _ctvp = (x); \
     Svectorp (_ctvp) && Svector_length (_ctvp) > 0 \
     && Sfixnump (Svector_ref (_ctvp, 0)) \
     && (Sfixnum_value (Svector_ref (_ctvp, 0)) & 0xFFF) == (tag); })

/* Extract just the base tag (low 12 bits) from slot 0.  */
#define CHEZ_VECTOR_TAG(x) \
  ({ Lisp_Object _cvt = (x); \
     Sfixnum_value (Svector_ref (_cvt, 0)) & 0xFFF; })

/* Extract the full tag value (for pvec_type encoding).  */
#define CHEZ_VECTOR_TAG_FULL(x) \
  ({ Lisp_Object _cvtf = (x); \
     Sfixnum_value (Svector_ref (_cvtf, 0)); })

#define lisp_h_CONSP(x)       Spairp (x)
#define lisp_h_FIXNUMP(x)     Sfixnump (x)
#define lisp_h_FLOATP(x)      Sflonump (x)
/* Nil can appear as either Snil (0x26, the Chez empty list) or as the
   actual nil symbol object (chez_symbols[iQnil]).  Both must be recognized.  */
#define lisp_h_NILP(x) \
  ({ Lisp_Object _nilp = (x); Snullp (_nilp) || _nilp == chez_symbols[0]; })
#define lisp_h_STRINGP(x)     CHEZ_TAGGED_VECTOR_P (x, CHEZ_STRING_TAG)
/* A bare symbol is either a tagged symbol vector, or Snil (for nil).
   Qt is now chez_symbols[1] (a symbol vector), so no Strue check needed.  */
#define lisp_h_BARE_SYMBOL_P(x) \
  ({ Lisp_Object _bsp = (x); \
     CHEZ_TAGGED_VECTOR_P (_bsp, CHEZ_SYMBOL_TAG) \
     || Snullp (_bsp); })
#define lisp_h_VECTORLIKEP(x) \
  ({ Lisp_Object _vlp = (x); \
     Svectorp (_vlp) && Svector_length (_vlp) > 0 \
     && Sfixnump (Svector_ref (_vlp, 0)) \
     && Sfixnum_value (Svector_ref (_vlp, 0)) >= CHEZ_VECTORLIKE_TAG; })

/* BASE_EQ: Pointer identity with dual-nil normalization.
   In Chez mode, nil has two representations: Snil (0x26, the empty list)
   and chez_symbols[0] (the nil symbol vector).  Both must be EQ.  */
#define lisp_h_BASE_EQ(x, y) \
  ({ Lisp_Object _eq_x = (x), _eq_y = (y); \
     _eq_x == _eq_y || (lisp_h_NILP (_eq_x) && lisp_h_NILP (_eq_y)); })

/* No tag-based type dispatch — these are unused but defined for compat.  */
#define lisp_h_TAGGEDP(a, tag) false

/* XTYPE: Return a Lisp_Type value for dispatching.
   Defined as int here because enum Lisp_Type is not yet declared.
   The actual chez_xtype function is defined later in lisp.h after
   the enum.  Use the macro for now which the function will replace.  */
#define lisp_h_XTYPE(a) chez_xtype_inline (a)

/* Use numeric Lisp_Type values matching the enum:
   Lisp_Symbol=0, Lisp_Int0=2, Lisp_Cons=3(LSB), Lisp_String=4,
   Lisp_Vectorlike=5, Lisp_Float=7.  */
INLINE int
chez_xtype_inline (Lisp_Object a)
{
  if (Sfixnump (a))
    return 2; /* Lisp_Int0 */
  if (Sflonump (a))
    return 7; /* Lisp_Float */
  if (Spairp (a))
    return 3; /* Lisp_Cons (USE_LSB_TAG=1) */
  if (CHEZ_TAGGED_VECTOR_P (a, CHEZ_SYMBOL_TAG)
      || Snullp (a))
    return 0; /* Lisp_Symbol */
  if (Svectorp (a) && Svector_length (a) > 0 && Sfixnump (Svector_ref (a, 0)))
    {
      iptr tag = Sfixnum_value (Svector_ref (a, 0)) & 0xFFF;
      if (tag == CHEZ_STRING_TAG)
	return 4; /* Lisp_String */
      return 5; /* Lisp_Vectorlike */
    }
  /* Fallback for other Chez immediates (booleans, void, etc.) */
  return 2; /* Lisp_Int0 */
}
#define lisp_h_XHASH(a) ((EMACS_UINT) (intptr_t) (a))
#define lisp_h_Qnil Snil

/* Forward-declare wrong_type_argument so CHECK_TYPE can reference it.
   The actual definition is in data.c.  */
extern _Noreturn void wrong_type_argument (Lisp_Object, Lisp_Object);

#define lisp_h_CHECK_TYPE(ok, predicate, x) \
  ((ok) ? (void) 0 : wrong_type_argument (predicate, x))

/* ====================================================================
   Fixnum constructors & accessors
   ==================================================================== */

#define lisp_h_make_fixnum_wrap(n) Sfixnum ((EMACS_INT)(n))
#define lisp_h_make_fixnum(n) Sfixnum ((EMACS_INT)(n))
#define lisp_h_XFIXNUM_RAW(a)  Sfixnum_value (a)

INLINE Lisp_Object
make_fixnum (EMACS_INT n)
{
  return Sfixnum (n);
}

INLINE Lisp_Object
make_ufixnum (EMACS_INT n)
{
  return Sfixnum (n);
}

INLINE EMACS_INT
XFIXNUM_RAW (Lisp_Object a)
{
  return Sfixnum_value (a);
}

INLINE EMACS_INT
XFIXNUM (Lisp_Object a)
{
  eassert (Sfixnump (a));
  return Sfixnum_value (a);
}

INLINE EMACS_UINT
XUFIXNUM_RAW (Lisp_Object a)
{
  return (EMACS_UINT) Sfixnum_value (a);
}

INLINE EMACS_UINT
XUFIXNUM (Lisp_Object a)
{
  eassert (Sfixnump (a));
  return (EMACS_UINT) Sfixnum_value (a);
}

INLINE bool
(FIXNUMP) (Lisp_Object x)
{
  return Sfixnump (x);
}

/* Fixnum limits.  Chez fixnums are at least 30 bits on 32-bit,
   62 bits on 64-bit.  Use Emacs's standard limits.  */
#define FIXNUM_OVERFLOW_P(i) \
  (! ((0 <= (i) || MOST_NEGATIVE_FIXNUM <= (i)) && (i) <= MOST_POSITIVE_FIXNUM))

/* ====================================================================
   Float
   ==================================================================== */

INLINE bool
(FLOATP) (Lisp_Object x)
{
  return Sflonump (x);
}

INLINE double
XFLOAT_DATA (Lisp_Object f)
{
  eassert (Sflonump (f));
  return Sflonum_value (f);
}

/* struct Lisp_Float compatibility — not a real struct with Chez,
   but some code may reference it.  Provide a minimal shim.  */
struct Lisp_Float
{
  union { double data; } u;
};

INLINE struct Lisp_Float *
XFLOAT (Lisp_Object a)
{
  /* This should not be called in Chez mode — use XFLOAT_DATA.
     Provide a stub that crashes if misused.  */
  abort ();
  return NULL;
}

/* ====================================================================
   Cons cells — Chez pairs
   ==================================================================== */

/* Chez cons cells are native pairs.  No struct Lisp_Cons in Chez mode,
   but we provide a minimal definition for code that references it.  */
struct Lisp_Cons
{
  union
  {
    struct
    {
      Lisp_Object car;
      union { Lisp_Object cdr; } u;
    } s;
  } u;
};

INLINE bool
(NILP) (Lisp_Object x)
{
  return Snullp (x) || x == chez_symbols[0];
}

INLINE bool
(CONSP) (Lisp_Object x)
{
  return Spairp (x);
}

/* CHECK_CONS defined later in lisp.h after CHECK_TYPE and Q* symbols.  */

/* XCONS should not be used in Chez mode — use XCAR/XCDR instead.
   Stub provided for compilation.  */
INLINE struct Lisp_Cons *
XCONS (Lisp_Object a)
{
  /* Cannot return a pointer to a Chez pair's internals.
     Code should use XCAR/XCDR instead.  */
  abort ();
  return NULL;
}

/* Cons cell field address — not available with Chez's copying GC.  */
INLINE Lisp_Object *
xcar_addr (Lisp_Object c)
{
  abort ();
  return NULL;
}

INLINE Lisp_Object *
xcdr_addr (Lisp_Object c)
{
  abort ();
  return NULL;
}

#define lisp_h_XCAR(c) Scar (c)
#define lisp_h_XCDR(c) Scdr (c)

INLINE Lisp_Object
(XCAR) (Lisp_Object c)
{
  return Scar (c);
}

INLINE Lisp_Object
(XCDR) (Lisp_Object c)
{
  return Scdr (c);
}

INLINE void
XSETCAR (Lisp_Object c, Lisp_Object n)
{
  Sset_car (c, n);
}

INLINE void
XSETCDR (Lisp_Object c, Lisp_Object n)
{
  Sset_cdr (c, n);
}

/* CAR, CDR, CAR_SAFE, CDR_SAFE defined later in lisp.h after
   wrong_type_argument and Q* symbols are available.  */

/* ====================================================================
   Strings — Chez vectors: [tag, bytevector, size_byte, intervals]
   ==================================================================== */

/* Minimal struct for compilation compat.  Should not be used directly.  */
struct Lisp_String
{
  union
  {
    struct
    {
      ptrdiff_t size;
      ptrdiff_t size_byte;
      void *intervals;
      unsigned char *data;
    } s;
    struct Lisp_String *next;
  } u;
};

INLINE bool
STRINGP (Lisp_Object x)
{
  return lisp_h_STRINGP (x);
}

/* CHECK_STRING defined later in lisp.h after CHECK_TYPE and Q* symbols.  */

/* XSTRING returns a pointer — cannot work with Chez vectors.
   Use SDATA/SBYTES/SCHARS instead.  */
INLINE struct Lisp_String *
XSTRING (Lisp_Object a)
{
  fprintf (stderr, "CHEZ: XSTRING called! value=%p\n", (void*)a);
  fflush (stderr);
  abort ();
  return NULL;
}

INLINE bool
STRING_MULTIBYTE (Lisp_Object str)
{
  eassert (STRINGP (str));
  iptr sb = Sfixnum_value (Svector_ref (str, CHEZ_STR_SLOT_SIZEBYTE));
  return sb >= 0;
}

INLINE unsigned char *
SDATA (Lisp_Object string)
{
  eassert (STRINGP (string));
  return (unsigned char *) Sbytevector_data (Svector_ref (string, CHEZ_STR_SLOT_DATA));
}

INLINE char *
SSDATA (Lisp_Object string)
{
  return (char *) SDATA (string);
}

INLINE unsigned char
SREF (Lisp_Object string, ptrdiff_t index)
{
  return SDATA (string)[index];
}

INLINE void
SSET (Lisp_Object string, ptrdiff_t index, unsigned char new_val)
{
  SDATA (string)[index] = new_val;
}

INLINE ptrdiff_t
SCHARS (Lisp_Object string)
{
  eassert (STRINGP (string));
  /* Bytevector has nbytes+1 (null terminator), so subtract 1.  */
  chez_ptr bv = Svector_ref (string, CHEZ_STR_SLOT_DATA);
  iptr size_byte = Sfixnum_value (Svector_ref (string, CHEZ_STR_SLOT_SIZEBYTE));
  if (size_byte < 0)
    return Sbytevector_length (bv) - 1;  /* unibyte: nchars == nbytes */
  /* multibyte: for now return nbytes (correct for ASCII).
     TODO: proper multibyte char count.  */
  return Sbytevector_length (bv) - 1;
}

INLINE ptrdiff_t
SBYTES (Lisp_Object string)
{
  if (!STRINGP (string))
    {
      fprintf (stderr, "CHEZ ABORT: SBYTES called with non-string %p Svectorp=%d\n",
	       (void*)string, Svectorp(string));
      if (Svectorp(string) && Svector_length(string) > 0)
	fprintf (stderr, "  slot0=%p fixnump=%d val=%ld\n",
		 (void*)Svector_ref(string,0),
		 Sfixnump(Svector_ref(string,0)),
		 Sfixnump(Svector_ref(string,0)) ? (long)Sfixnum_value(Svector_ref(string,0)) : -1);
      fflush (stderr);
      abort ();
    }
  iptr size_byte = Sfixnum_value (Svector_ref (string, CHEZ_STR_SLOT_SIZEBYTE));
  if (size_byte < 0)
    /* Unibyte string: size_byte is -1, actual byte count is
       bytevector length minus 1 (null terminator).  */
    return Sbytevector_length (Svector_ref (string, CHEZ_STR_SLOT_DATA)) - 1;
  return size_byte;
}

INLINE ptrdiff_t
STRING_BYTES (struct Lisp_String *s)
{
  /* Should not be called in Chez mode.  */
  abort ();
  return 0;
}

INLINE void
STRING_SET_CHARS (Lisp_Object string, ptrdiff_t newsize)
{
  /* Adjust size metadata.  Chez bytevector size is immutable,
     so this only works if newsize <= current size.  */
  (void) string;
  (void) newsize;
}

/* ====================================================================
   Symbols — Chez vectors: [tag|flags, name, value, function, plist, next]
   ==================================================================== */

/* Emacs symbol flags (stored in high bits of slot 0).  */
enum symbol_interned
  {
    SYMBOL_UNINTERNED = 0,
    SYMBOL_INTERNED = 1,
    SYMBOL_INTERNED_IN_INITIAL_OBARRAY = 2,
  };

enum symbol_redirect
  {
    SYMBOL_PLAINVAL  = 0,
    SYMBOL_VARALIAS  = 1,
    SYMBOL_LOCALIZED = 2,
    SYMBOL_FORWARDED = 3,
  };

enum symbol_trapped_write
  {
    SYMBOL_UNTRAPPED_WRITE = 0,
    SYMBOL_NOWRITE = 1,
    SYMBOL_TRAPPED_WRITE = 2,
  };

/* Minimal struct for compilation.  Not used at runtime in Chez mode.
   Symbols are Chez vectors accessed via SYMBOL_NAME etc.  */
struct Lisp_Symbol
{
  union
  {
    struct
    {
      unsigned int redirect : 3;
      unsigned int declared_special : 1;
      unsigned int trapped_write : 2;
      unsigned int interned : 2;
      unsigned int c_variable : 1;
      unsigned int pinned : 1;
      Lisp_Object name;
      union
      {
        Lisp_Object value;
        struct Lisp_Symbol *alias;
        void *blv;
        struct Lisp_Fwd *fwd;
      } val;
      Lisp_Object function;
      Lisp_Object plist;
      struct Lisp_Symbol *next;
    } s;
  } u;
};

/* Flag bit positions within the tag fixnum (slot 0).
   Low 12 bits: type tag (CHEZ_SYMBOL_TAG = 0x200)
   Bits 12-14: redirect (3 bits)
   Bit 15: declared_special
   Bits 16-17: trapped_write (2 bits)
   Bits 18-19: interned (2 bits)
   Bit 20: pinned  */
#define CHEZ_SYM_FLAG_REDIRECT_SHIFT    12
#define CHEZ_SYM_FLAG_SPECIAL_BIT       (1 << 15)
#define CHEZ_SYM_FLAG_TRAPPED_SHIFT     16
#define CHEZ_SYM_FLAG_INTERNED_SHIFT    18
#define CHEZ_SYM_FLAG_PINNED_BIT        (1 << 20)

INLINE bool
(BARE_SYMBOL_P) (Lisp_Object x)
{
  return lisp_h_BARE_SYMBOL_P (x);
}

INLINE bool
SYMBOLP (Lisp_Object x)
{
  return BARE_SYMBOL_P (x);
}

/* CHECK_SYMBOL defined later in lisp.h after CHECK_TYPE and Q* symbols.  */

/* In Chez mode, XSYMBOL returns NULL — code should use the
   SYMBOL_NAME / SYMBOL_VAL / etc. accessors instead.
   Some compatibility code may still reference it.  */
INLINE _Noreturn void
chez_xsymbol_abort (const char *file, int line)
{
  fprintf (stderr, "CHEZ ABORT: XBARE_SYMBOL/XSYMBOL called at %s:%d\n",
	   file, line);
  fflush (stderr);
  abort ();
}

#define XBARE_SYMBOL(a) (chez_xsymbol_abort (__FILE__, __LINE__), (struct Lisp_Symbol *) NULL)
#define XSYMBOL(a)      (chez_xsymbol_abort (__FILE__, __LINE__), (struct Lisp_Symbol *) NULL)

/* Symbol field accessors that work on Chez vectors.
   Use chez_resolve_symbol to handle Snil (→ chez_nil_symbol)
   since Snil is an immediate, not a vector.  Strue/Qt is already
   a proper symbol vector, so it passes through unchanged.  */
INLINE Lisp_Object
SYMBOL_NAME (Lisp_Object sym)
{
  eassert (SYMBOLP (sym));
  return Svector_ref (chez_resolve_symbol (sym), CHEZ_SYM_SLOT_NAME);
}

INLINE Lisp_Object
SYMBOL_VAL (Lisp_Object sym)
{
  eassert (SYMBOLP (sym));
  Lisp_Object val = Svector_ref (chez_resolve_symbol (sym), CHEZ_SYM_SLOT_VAL);
  /* Chez symbols use Svoid as the unbound marker in the value slot.
     Map it to Qunbound (= chez_symbols[iQunbound]) so
     BASE_EQ(val, Qunbound) checks work throughout Emacs.  */
  if (val == Svoid)
    return chez_symbols[2];  /* iQunbound = 2 */
  return val;
}

INLINE Lisp_Object
SYMBOL_FUNCTION (Lisp_Object sym)
{
  eassert (SYMBOLP (sym));
  return Svector_ref (chez_resolve_symbol (sym), CHEZ_SYM_SLOT_FUNC);
}

INLINE Lisp_Object
SYMBOL_PLIST (Lisp_Object sym)
{
  eassert (SYMBOLP (sym));
  return Svector_ref (chez_resolve_symbol (sym), CHEZ_SYM_SLOT_PLIST);
}

INLINE void
SET_SYMBOL_VAL (Lisp_Object sym, Lisp_Object val)
{
  eassert (SYMBOLP (sym));
  /* Map Qunbound back to Svoid for internal storage.  */
  if (val == chez_symbols[2])  /* iQunbound = 2 */
    val = Svoid;
  Svector_set (chez_resolve_symbol (sym), CHEZ_SYM_SLOT_VAL, val);
}

INLINE void
SET_SYMBOL_FUNCTION (Lisp_Object sym, Lisp_Object func)
{
  eassert (SYMBOLP (sym));
  Svector_set (chez_resolve_symbol (sym), CHEZ_SYM_SLOT_FUNC, func);
}

INLINE void
SET_SYMBOL_PLIST (Lisp_Object sym, Lisp_Object plist)
{
  eassert (SYMBOLP (sym));
  Svector_set (chez_resolve_symbol (sym), CHEZ_SYM_SLOT_PLIST, plist);
}

INLINE iptr
chez_sym_flags (Lisp_Object sym)
{
  return Sfixnum_value (Svector_ref (chez_resolve_symbol (sym), CHEZ_SYM_SLOT_TAG));
}

INLINE enum symbol_redirect
SYMBOL_REDIRECT (Lisp_Object sym)
{
  return (enum symbol_redirect)
    ((chez_sym_flags (sym) >> CHEZ_SYM_FLAG_REDIRECT_SHIFT) & 7);
}

INLINE bool
SYMBOL_CONSTANT_P (Lisp_Object sym)
{
  return ((chez_sym_flags (sym) >> CHEZ_SYM_FLAG_TRAPPED_SHIFT) & 3)
         == SYMBOL_NOWRITE;
}

#define lisp_h_SYMBOL_CONSTANT_P(sym) SYMBOL_CONSTANT_P (sym)
#define lisp_h_SYMBOL_TRAPPED_WRITE_P(sym) \
  ((enum symbol_trapped_write) ((chez_sym_flags (sym) >> CHEZ_SYM_FLAG_TRAPPED_SHIFT) & 3))
#define lisp_h_SYMBOL_WITH_POS_P(x) false

INLINE enum symbol_trapped_write
SYMBOL_TRAPPED_WRITE_P (Lisp_Object sym)
{
  return lisp_h_SYMBOL_TRAPPED_WRITE_P (sym);
}

/* Symbols with position — not supported in Chez mode.  */
INLINE bool
SYMBOL_WITH_POS_P (Lisp_Object x)
{
  return false;
}

/* ====================================================================
   Vectorlike objects — Chez vectors with tag in slot 0
   ==================================================================== */

/* In standard Emacs, vectorlike objects include vectors and pseudovectors.
   In Chez mode, these are all Chez vectors with a type tag in slot 0.

   Regular vectors: slot 0 = CHEZ_VECTORLIKE_TAG, slots 1..N = elements
   Pseudovectors:   slot 0 = CHEZ_PVEC_BASE | (pvec_type << 8) | nfields  */

INLINE bool
(VECTORLIKEP) (Lisp_Object x)
{
  return lisp_h_VECTORLIKEP (x);
}

INLINE bool
VECTORP (Lisp_Object x)
{
  return CHEZ_TAGGED_VECTOR_P (x, CHEZ_VECTORLIKE_TAG);
}

/* Pseudovector type check.  */
INLINE bool
chez_pseudovectorp (Lisp_Object x, int pvec_type)
{
  if (!Svectorp (x) || Svector_length (x) < 1)
    return false;
  chez_ptr tag_obj = Svector_ref (x, 0);
  if (!Sfixnump (tag_obj))
    return false;
  iptr tag = Sfixnum_value (tag_obj);
  return tag == (CHEZ_PVEC_BASE + pvec_type);
}

#define PSEUDOVECTORP(x, code) chez_pseudovectorp (x, code)

/* Vector length — number of Lisp elements (excluding tag slot).  */
INLINE ptrdiff_t
ASIZE (Lisp_Object array)
{
  eassert (VECTORP (array));
  return Svector_length (array) - 1;  /* subtract tag slot */
}

/* Pointer to the first Lisp element of a Chez vector (slot 1).
   This allows memcpy-based bulk operations on vector contents.
   Slot 0 is the type tag, so Lisp elements start at slot 1.  */
#define XVECTOR_CONTENTS(v) (&Svector_ref ((v), 1))

/* Vector element access.  Slot 0 is the tag, so element i is slot i+1.  */
INLINE Lisp_Object
AREF (Lisp_Object array, ptrdiff_t idx)
{
  eassert (VECTORP (array));
  return Svector_ref (array, idx + 1);
}

INLINE void
ASET (Lisp_Object array, ptrdiff_t idx, Lisp_Object val)
{
  eassert (VECTORP (array));
  Svector_set (array, idx + 1, val);
}

/* ====================================================================
   Bignum — use Chez native bignums
   ==================================================================== */

INLINE bool
BIGNUMP (Lisp_Object x)
{
  /* PVEC_BIGNUM = 2 (from enum pvec_type, not yet declared here).  */
  return Sbignump (x) || chez_pseudovectorp (x, 2);
}

INLINE bool
INTEGERP (Lisp_Object x)
{
  return FIXNUMP (x) || BIGNUMP (x);
}

/* ====================================================================
   BASE_EQ and EQ
   ==================================================================== */

INLINE bool
(BASE_EQ) (Lisp_Object x, Lisp_Object y)
{
  return x == y || (NILP (x) && NILP (y));
}

INLINE bool
EQ (Lisp_Object x, Lisp_Object y)
{
  return x == y || (NILP (x) && NILP (y));
}

/* Needed for the hashing system.  */
INLINE EMACS_UINT
(XHASH) (Lisp_Object a)
{
  return (EMACS_UINT) (intptr_t) a;
}

/* ====================================================================
   Miscellaneous compatibility
   ==================================================================== */

/* Emacs_Int / Lisp_Object are both pointer-sized.  */
#define VALMASK ((EMACS_INT) -1)
#define INTTYPEBITS 0
#define VALBITS (EMACS_INT_WIDTH)

/* Fixnum range — use full Chez fixnum range.  */
#define MOST_POSITIVE_FIXNUM (EMACS_INT_MAX >> 3)
#define MOST_NEGATIVE_FIXNUM (-1 - MOST_POSITIVE_FIXNUM)
#define INTMASK MOST_POSITIVE_FIXNUM

/* The Lisp_Type enum is not used in Chez mode, but some code
   references it.  Provide stub values.  */
enum Lisp_Type
  {
    Lisp_Symbol = 0,
    Lisp_Type_Unused0 = 1,
    Lisp_Int0 = 2,
    Lisp_Int1 = 3,
    Lisp_String = 4,
    Lisp_Vectorlike = 5,
    Lisp_Cons = 6,
    Lisp_Float = 7,
  };

/* Not meaningful in Chez mode.  */
#define USE_LSB_TAG 0

/* Forwarding types — kept for buffer-local variable support.  */
enum Lisp_Fwd_Type
  {
    Lisp_Fwd_Int,
    Lisp_Fwd_Bool,
    Lisp_Fwd_Obj,
    Lisp_Fwd_Buffer_Obj,
    Lisp_Fwd_Kboard_Obj,
  };

/* XUNTAG — extract C struct pointer from a Lisp_Object.
   Handles both wrapped Chez vectors (slot 1 = ptr as fixnum)
   and raw C pointers cast to Lisp_Object.  */
INLINE void *
chez_xuntag (Lisp_Object a)
{
  if (Svectorp (a) && Svector_length (a) >= 2
      && Sfixnump (Svector_ref (a, 1)))
    return (void *)(iptr) Sfixnum_value (Svector_ref (a, 1));
  return (void *) a;
}
#define XUNTAG(a, type, ctype) ((ctype *) chez_xuntag (a))

/* make_lisp_ptr — stores a C pointer as a Lisp_Object.
   For vectorlike types, the pointer is stored as a raw cast.
   Callers that need proper tagging (make_lisp_obarray, etc.) should
   use chez_wrap_c_pseudovector() defined later in lisp.h.  */
INLINE Lisp_Object
make_lisp_ptr (void *ptr_val, enum Lisp_Type type)
{
  (void) type;
  return (Lisp_Object) ptr_val;
}

/* ====================================================================
   DEFINE_KEY_OPS_AS_MACROS — define them all for Chez
   ==================================================================== */

#define DEFINE_KEY_OPS_AS_MACROS 1
#define CONSP(x)       lisp_h_CONSP (x)
#define BASE_EQ(x, y)  lisp_h_BASE_EQ (x, y)
#define FLOATP(x)      lisp_h_FLOATP (x)
#define FIXNUMP(x)     lisp_h_FIXNUMP (x)
#define NILP(x)        lisp_h_NILP (x)
#define TAGGEDP(a, tag) false
#define STRINGP(x)     lisp_h_STRINGP (x)
#define VECTORLIKEP(x) lisp_h_VECTORLIKEP (x)
#define XCAR(c)        lisp_h_XCAR (c)
#define XCDR(c)        lisp_h_XCDR (c)
#define XHASH(a)       lisp_h_XHASH (a)
#define make_fixnum(n)  lisp_h_make_fixnum (n)
#define XFIXNUM_RAW(a) lisp_h_XFIXNUM_RAW (a)
#define XTYPE(a)       lisp_h_XTYPE (a)
#define BARE_SYMBOL_P(x) lisp_h_BARE_SYMBOL_P (x)
#define SYMBOL_CONSTANT_P(sym) lisp_h_SYMBOL_CONSTANT_P (sym)
#define SYMBOL_TRAPPED_WRITE_P(sym) lisp_h_SYMBOL_TRAPPED_WRITE_P (sym)
#define SYMBOL_WITH_POS_P(x) false

/* Symbol-with-pos stubs.  SYMBOL_WITH_POS_P is always false, so these
   are never called, but the compiler needs them to exist.  */
INLINE Lisp_Object
XSYMBOL_WITH_POS_SYM (Lisp_Object a)
{
  (void) a;
  abort ();
  return Qnil;
}

INLINE Lisp_Object
XSYMBOL_WITH_POS_POS (Lisp_Object a)
{
  (void) a;
  abort ();
  return Qnil;
}

INLINE Lisp_Object
build_symbol_with_pos (Lisp_Object sym, Lisp_Object pos)
{
  (void) sym; (void) pos;
  abort ();
  return Qnil;
}

struct Lisp_Symbol_With_Pos;

INLINE struct Lisp_Symbol_With_Pos *
XSYMBOL_WITH_POS (Lisp_Object a)
{
  (void) a;
  abort ();
  return NULL;
}

/* lispsym — not used in Chez mode; symbols are heap-allocated.
   Provide a stub to avoid compilation errors.  */
extern struct Lisp_Symbol lispsym[];
#define builtin_lisp_symbol(i) chez_symbols[i]

/* symbols_with_pos_enabled — not supported.  */
#define symbols_with_pos_enabled false

/* GC pinning macros for C locals that must survive across calls
   which may trigger GC (eval_sub → Fload → readevalloop → GC).

   Symbols are permanently pinned at intern time and must NEVER be
   unlocked — these macros skip them.  Immediates (fixnums, nil,
   booleans, void, chars) are also skipped (not heap-allocated).  */
#define chez_gc_pin(x)					\
  do { Lisp_Object _pin = (x);				\
    if (!SYMBOLP (_pin) && !Sfixnump (_pin)		\
	&& _pin != Svoid && !Scharp (_pin))		\
      Slock_object (_pin);				\
  } while (0)

#define chez_gc_unpin(x)				\
  do { Lisp_Object _unpin = (x);			\
    if (!SYMBOLP (_unpin) && !Sfixnump (_unpin)		\
	&& _unpin != Svoid && !Scharp (_unpin))		\
      Sunlock_object (_unpin);				\
  } while (0)

#endif /* EMACS_LISP_CHEZ_H */
