/* Bridge between Emacs and Chez Scheme library functions.
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

/* This file includes scheme.h WITHOUT renaming, so the real function
   names (Scons, Sstring, Smake_string, Smake_vector) are available.
   It provides wrapper functions that chez.h's macros call.  */

#include <config.h>

#ifdef HAVE_CHEZ

#include <scheme.h>
#include <stdio.h>
#include <stdlib.h>

ptr
chez_Scons_real (ptr a, ptr b)
{
  return Scons (a, b);
}

ptr
chez_Sstring_real (const char *s)
{
  return Sstring (s);
}

ptr
chez_Smake_string_real (iptr n, int c)
{
  return Smake_string (n, c);
}

ptr
chez_Smake_vector_real (iptr n, ptr v)
{
  return Smake_vector (n, v);
}

/* Write-barrier wrappers.  The real Sset_car, Sset_cdr, and Svector_set
   library functions go through SETCAR/SETCDR/SETVECTIT macros which
   expand to DIRTYSET → S_dirty_set.  This maintains the dirty card
   table that the generational GC needs to find inter-generational
   references during dirty sweep.

   chez.h previously redefined these as direct memory stores (for
   ptr naming conflict avoidance), which completely bypassed the write
   barrier — causing gen-0 objects referenced from gen-1 to become
   stale after collection.  */

void
chez_set_car_wb (ptr p, ptr x)
{
  Sset_car (p, x);
}

void
chez_set_cdr_wb (ptr p, ptr x)
{
  Sset_cdr (p, x);
}

void
chez_vector_set_wb (ptr v, iptr i, ptr x)
{
  Svector_set (v, i, x);
}

#endif /* HAVE_CHEZ */
