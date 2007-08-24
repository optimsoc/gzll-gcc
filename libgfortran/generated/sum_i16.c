/* Implementation of the SUM intrinsic
   Copyright 2002 Free Software Foundation, Inc.
   Contributed by Paul Brook <paul@nowt.org>

This file is part of the GNU Fortran 95 runtime library (libgfortran).

Libgfortran is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

In addition to the permissions in the GNU General Public License, the
Free Software Foundation gives you unlimited permission to link the
compiled version of this file into combinations with other programs,
and to distribute those combinations without any restriction coming
from the use of this file.  (The General Public License restrictions
do apply in other respects; for example, they cover modification of
the file, and distribution when not linked into a combine
executable.)

Libgfortran is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public
License along with libgfortran; see the file COPYING.  If not,
write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
Boston, MA 02110-1301, USA.  */

#include "config.h"
#include <stdlib.h>
#include <assert.h>
#include "libgfortran.h"


#if defined (HAVE_GFC_INTEGER_16) && defined (HAVE_GFC_INTEGER_16)


extern void sum_i16 (gfc_array_i16 * const restrict, 
	gfc_array_i16 * const restrict, const index_type * const restrict);
export_proto(sum_i16);

void
sum_i16 (gfc_array_i16 * const restrict retarray, 
	gfc_array_i16 * const restrict array, 
	const index_type * const restrict pdim)
{
  index_type count[GFC_MAX_DIMENSIONS];
  index_type extent[GFC_MAX_DIMENSIONS];
  index_type sstride[GFC_MAX_DIMENSIONS];
  index_type dstride[GFC_MAX_DIMENSIONS];
  const GFC_INTEGER_16 * restrict base;
  GFC_INTEGER_16 * restrict dest;
  index_type rank;
  index_type n;
  index_type len;
  index_type delta;
  index_type dim;

  /* Make dim zero based to avoid confusion.  */
  dim = (*pdim) - 1;
  rank = GFC_DESCRIPTOR_RANK (array) - 1;

  len = array->dim[dim].ubound + 1 - array->dim[dim].lbound;
  delta = array->dim[dim].stride;

  for (n = 0; n < dim; n++)
    {
      sstride[n] = array->dim[n].stride;
      extent[n] = array->dim[n].ubound + 1 - array->dim[n].lbound;

      if (extent[n] < 0)
	extent[n] = 0;
    }
  for (n = dim; n < rank; n++)
    {
      sstride[n] = array->dim[n + 1].stride;
      extent[n] =
        array->dim[n + 1].ubound + 1 - array->dim[n + 1].lbound;

      if (extent[n] < 0)
	extent[n] = 0;
    }

  if (retarray->data == NULL)
    {
      size_t alloc_size;

      for (n = 0; n < rank; n++)
        {
          retarray->dim[n].lbound = 0;
          retarray->dim[n].ubound = extent[n]-1;
          if (n == 0)
            retarray->dim[n].stride = 1;
          else
            retarray->dim[n].stride = retarray->dim[n-1].stride * extent[n-1];
        }

      retarray->offset = 0;
      retarray->dtype = (array->dtype & ~GFC_DTYPE_RANK_MASK) | rank;

      alloc_size = sizeof (GFC_INTEGER_16) * retarray->dim[rank-1].stride
    		   * extent[rank-1];

      if (alloc_size == 0)
	{
	  /* Make sure we have a zero-sized array.  */
	  retarray->dim[0].lbound = 0;
	  retarray->dim[0].ubound = -1;
	  return;
	}
      else
	retarray->data = internal_malloc_size (alloc_size);
    }
  else
    {
      if (rank != GFC_DESCRIPTOR_RANK (retarray))
	runtime_error ("rank of return array incorrect");
    }

  for (n = 0; n < rank; n++)
    {
      count[n] = 0;
      dstride[n] = retarray->dim[n].stride;
      if (extent[n] <= 0)
        len = 0;
    }

  base = array->data;
  dest = retarray->data;

  while (base)
    {
      const GFC_INTEGER_16 * restrict src;
      GFC_INTEGER_16 result;
      src = base;
      {

  result = 0;
        if (len <= 0)
	  *dest = 0;
	else
	  {
	    for (n = 0; n < len; n++, src += delta)
	      {

  result += *src;
          }
	    *dest = result;
	  }
      }
      /* Advance to the next element.  */
      count[0]++;
      base += sstride[0];
      dest += dstride[0];
      n = 0;
      while (count[n] == extent[n])
        {
          /* When we get to the end of a dimension, reset it and increment
             the next dimension.  */
          count[n] = 0;
          /* We could precalculate these products, but this is a less
             frequently used path so probably not worth it.  */
          base -= sstride[n] * extent[n];
          dest -= dstride[n] * extent[n];
          n++;
          if (n == rank)
            {
              /* Break out of the look.  */
              base = NULL;
              break;
            }
          else
            {
              count[n]++;
              base += sstride[n];
              dest += dstride[n];
            }
        }
    }
}


extern void msum_i16 (gfc_array_i16 * const restrict, 
	gfc_array_i16 * const restrict, const index_type * const restrict,
	gfc_array_l1 * const restrict);
export_proto(msum_i16);

void
msum_i16 (gfc_array_i16 * const restrict retarray, 
	gfc_array_i16 * const restrict array, 
	const index_type * const restrict pdim, 
	gfc_array_l1 * const restrict mask)
{
  index_type count[GFC_MAX_DIMENSIONS];
  index_type extent[GFC_MAX_DIMENSIONS];
  index_type sstride[GFC_MAX_DIMENSIONS];
  index_type dstride[GFC_MAX_DIMENSIONS];
  index_type mstride[GFC_MAX_DIMENSIONS];
  GFC_INTEGER_16 * restrict dest;
  const GFC_INTEGER_16 * restrict base;
  const GFC_LOGICAL_1 * restrict mbase;
  int rank;
  int dim;
  index_type n;
  index_type len;
  index_type delta;
  index_type mdelta;
  int mask_kind;

  dim = (*pdim) - 1;
  rank = GFC_DESCRIPTOR_RANK (array) - 1;

  len = array->dim[dim].ubound + 1 - array->dim[dim].lbound;
  if (len <= 0)
    return;

  mbase = mask->data;

  mask_kind = GFC_DESCRIPTOR_SIZE (mask);

  if (mask_kind == 1 || mask_kind == 2 || mask_kind == 4 || mask_kind == 8
#ifdef HAVE_GFC_LOGICAL_16
      || mask_kind == 16
#endif
      )
    mbase = GFOR_POINTER_TO_L1 (mbase, mask_kind);
  else
    runtime_error ("Funny sized logical array");

  delta = array->dim[dim].stride;
  mdelta = mask->dim[dim].stride * mask_kind;

  for (n = 0; n < dim; n++)
    {
      sstride[n] = array->dim[n].stride;
      mstride[n] = mask->dim[n].stride * mask_kind;
      extent[n] = array->dim[n].ubound + 1 - array->dim[n].lbound;

      if (extent[n] < 0)
	extent[n] = 0;

    }
  for (n = dim; n < rank; n++)
    {
      sstride[n] = array->dim[n + 1].stride;
      mstride[n] = mask->dim[n + 1].stride * mask_kind;
      extent[n] =
        array->dim[n + 1].ubound + 1 - array->dim[n + 1].lbound;

      if (extent[n] < 0)
	extent[n] = 0;
    }

  if (retarray->data == NULL)
    {
      size_t alloc_size;

      for (n = 0; n < rank; n++)
        {
          retarray->dim[n].lbound = 0;
          retarray->dim[n].ubound = extent[n]-1;
          if (n == 0)
            retarray->dim[n].stride = 1;
          else
            retarray->dim[n].stride = retarray->dim[n-1].stride * extent[n-1];
        }

      alloc_size = sizeof (GFC_INTEGER_16) * retarray->dim[rank-1].stride
    		   * extent[rank-1];

      retarray->offset = 0;
      retarray->dtype = (array->dtype & ~GFC_DTYPE_RANK_MASK) | rank;

      if (alloc_size == 0)
	{
	  /* Make sure we have a zero-sized array.  */
	  retarray->dim[0].lbound = 0;
	  retarray->dim[0].ubound = -1;
	  return;
	}
      else
	retarray->data = internal_malloc_size (alloc_size);

    }
  else
    {
      if (rank != GFC_DESCRIPTOR_RANK (retarray))
	runtime_error ("rank of return array incorrect");
    }

  for (n = 0; n < rank; n++)
    {
      count[n] = 0;
      dstride[n] = retarray->dim[n].stride;
      if (extent[n] <= 0)
        return;
    }

  dest = retarray->data;
  base = array->data;

  while (base)
    {
      const GFC_INTEGER_16 * restrict src;
      const GFC_LOGICAL_1 * restrict msrc;
      GFC_INTEGER_16 result;
      src = base;
      msrc = mbase;
      {

  result = 0;
        if (len <= 0)
	  *dest = 0;
	else
	  {
	    for (n = 0; n < len; n++, src += delta, msrc += mdelta)
	      {

  if (*msrc)
    result += *src;
              }
	    *dest = result;
	  }
      }
      /* Advance to the next element.  */
      count[0]++;
      base += sstride[0];
      mbase += mstride[0];
      dest += dstride[0];
      n = 0;
      while (count[n] == extent[n])
        {
          /* When we get to the end of a dimension, reset it and increment
             the next dimension.  */
          count[n] = 0;
          /* We could precalculate these products, but this is a less
             frequently used path so probably not worth it.  */
          base -= sstride[n] * extent[n];
          mbase -= mstride[n] * extent[n];
          dest -= dstride[n] * extent[n];
          n++;
          if (n == rank)
            {
              /* Break out of the look.  */
              base = NULL;
              break;
            }
          else
            {
              count[n]++;
              base += sstride[n];
              mbase += mstride[n];
              dest += dstride[n];
            }
        }
    }
}


extern void ssum_i16 (gfc_array_i16 * const restrict, 
	gfc_array_i16 * const restrict, const index_type * const restrict,
	GFC_LOGICAL_4 *);
export_proto(ssum_i16);

void
ssum_i16 (gfc_array_i16 * const restrict retarray, 
	gfc_array_i16 * const restrict array, 
	const index_type * const restrict pdim, 
	GFC_LOGICAL_4 * mask)
{
  index_type rank;
  index_type n;
  index_type dstride;
  GFC_INTEGER_16 *dest;

  if (*mask)
    {
      sum_i16 (retarray, array, pdim);
      return;
    }
    rank = GFC_DESCRIPTOR_RANK (array);
  if (rank <= 0)
    runtime_error ("Rank of array needs to be > 0");

  if (retarray->data == NULL)
    {
      retarray->dim[0].lbound = 0;
      retarray->dim[0].ubound = rank-1;
      retarray->dim[0].stride = 1;
      retarray->dtype = (retarray->dtype & ~GFC_DTYPE_RANK_MASK) | 1;
      retarray->offset = 0;
      retarray->data = internal_malloc_size (sizeof (GFC_INTEGER_16) * rank);
    }
  else
    {
      if (GFC_DESCRIPTOR_RANK (retarray) != 1)
	runtime_error ("rank of return array does not equal 1");

      if (retarray->dim[0].ubound + 1 - retarray->dim[0].lbound != rank)
        runtime_error ("dimension of return array incorrect");
    }

    dstride = retarray->dim[0].stride;
    dest = retarray->data;

    for (n = 0; n < rank; n++)
      dest[n * dstride] = 0 ;
}

#endif
