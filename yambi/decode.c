/*-
  Copyright (C) 2011 Mikolaj Izdebski.  All rights reserved.

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "decode.h"


/* Implementation of Sliding Lists algorithm for doing Inverse Move-To-Front
   (IMTF) transformation in O(n) space and amortised O(sqrt(n)) time.
   The naive IMTF algorithm does the same in both O(n) space and time.
   Generally IMTF can be done in O(log(n)) time, but a quite big constant
   factor makes such algorithms impractical for MTF of 256 items.

   TODO: add more comments before I forget how this worked :)
*/
static Byte
mtf_one(YBdec_t *state, Byte c)
{
  Byte *pp;

  /* We expect the index to be small, so we have a special case for that. */
  if (likely(c < IMTF_ROW_WIDTH))
  {
    Int nn = c;
    pp = state->imtf_row[0];
    c = pp[nn];

    /* Forgive me the ugliness of this code, but mtf_one() is executed
       frequently and needs to be fast.  An eqivalent (simpler and slower)
       version is given in #else clause.
    */
#if IMTF_ROW_WIDTH == 16
    switch (nn) {
#define R(n) case n: pp[n] = pp[n-1]
      R(15); R(14); R(13); R(12); R(11); R(10); R( 9);
      R( 8); R( 7); R( 6); R( 5); R( 4); R( 3); R( 2); R( 1);
#undef R
    }
#else
    while (nn > 0)
    {
      pp[nn] = pp[nn-1];
      nn--;
    }
#endif
  }
  else  /* A general case for indices >= IMTF_ROW_WIDTH. */
  {
    /* If the sliding list already reached the bottom of memory pool
       allocated for it, we need to rebuild it. */
    if (unlikely(state->imtf_row[0] == state->imtf_slide))
    {
      Byte *kk = state->imtf_slide + IMTF_SLIDE_LENGTH;
      Byte **rr = state->imtf_row + IMTF_NUM_ROWS;

      while (rr > state->imtf_row)
      {
        Byte *bg = *--rr;
        Byte *bb = bg + IMTF_ROW_WIDTH;

        assert(bg >= state->imtf_slide &&
               bb <= state->imtf_slide + IMTF_SLIDE_LENGTH);

        while (bb > bg)
          *--kk = *--bb;
        *rr = kk;
      }
    }

    {
      Byte **lno = state->imtf_row + c / IMTF_ROW_WIDTH;
      Byte *bb = *lno;

      pp = bb + c % IMTF_ROW_WIDTH;
      c = *pp;

      while (pp > bb)
      {
        Byte *tt = pp--;
        *tt = *pp;
      }

      while (lno > state->imtf_row)
      {
        Byte **lno1 = lno;
        pp = --(*--lno);
        **lno1 = pp[IMTF_ROW_WIDTH];
      }
    }
  }

  *pp = c;
  return c;
}


ptrdiff_t
YBdec_work(YBdec_t *state)
{
  Short s;
  Int i, j = 0;
  Int cum;
  Byte uc;

  Byte runChar = state->imtf_slide[IMTF_SLIDE_LENGTH - 256];
  Int shift = 0;
  Int r = 0;

  Int ftab[256];  /* frequency table used in counting sort */

  /* Initialise IBWT frequency table. */
  for (s = 0; s < 256; s++)
    ftab[s] = 0;

  /* Initialise IMTF decoding structure. */
  for (i = 0; i < IMTF_NUM_ROWS; i++)
    state->imtf_row[i] = state->imtf_slide + IMTF_SLIDE_LENGTH - 256
      + i * IMTF_ROW_WIDTH;


  for (i = 0; i < state->num_mtfv; i++)
  {
    s = state->tt16[i];

    /* If we decoded a RLE symbol, increase run length and keep going.
       However, we need to stop accepting RLE symbols if the run gets
       too long.  Note that rejcting further RLE symbols after the run
       has reached the length of 900k bytes is perfectly correct because
       runs longer than 900k bytes will cause block overflow anyways
       and hence stop decoding with an error. */
    if (s >= 256 && r <= 900000)
    {
      r += (s & 3) << shift++;
      continue;
    }

    /* At this point we most likely have a run of one or more bytes.
       Zero-length run is possible only at the beginning, once per block,
       so any optimizations inolving zero-length runs are pointless. */
    if (unlikely(j + r > state->ibs->max_block_size))
      return -1;
    ftab[runChar] += r;
    while (r--)
      state->tt[j++] = runChar;

    runChar = mtf_one(state, s);
    shift = 0;
    r = 1;
  }

  /* At this point we must have an unfinished run, let's finish it. */
  assert(r > 0);
  if (unlikely(j + r > state->ibs->max_block_size))
    return -100;
  ftab[runChar] += r;
  while (r--)
    state->tt[j++] = runChar;

  assert(j >= state->num_mtfv);
  state->block_size = j;

  /* Sanity-check the BWT primary index. */
  if (state->bwt_idx >= state->block_size)
    return -1;

  /* Transform counts into indices (cumulative counts). */
  cum = 0;
  for (i = 0; i < 256; i++)
    ftab[i] = (cum += ftab[i]) - ftab[i];
  assert(cum == state->block_size);


  /* Construct the IBWT singly-linked cyclic list.  Traversing that list
     starting at primary index produces the source string.

     Each list node consists of a pointer to the next node and a character
     of the source string.  Those 2 values are packed into a single 32bit
     integer.  The character is kept in bits 0-7 and the pointer in stored
     in bits 8-27.  Bits 28-31 are unused (always clear).

     Note: Iff the source string consists of a string repeated k times
     (eg. ABABAB - the string AB is repeated k=3 times) then this algorithm
     will construct k independant (not connected), isomorphic lists.
  */
  for (i = 0; i < state->block_size; i++)
  {
    uc = state->tt[i];
    state->tt[ftab[uc]] |= (i << 8);
    ftab[uc]++;
  }
  assert(ftab[255] == state->block_size);

  state->rle_index = state->tt[state->bwt_idx];
  state->rle_avail = state->block_size;

  /* FIXME: So far there is no support for ramdomized blocks !!! */
  if (state->rand)
    return -100;

  return 0;
}


/* Local Variables: */
/* mode: C */
/* c-file-style: "bsd" */
/* c-basic-offset: 2 */
/* indent-tabs-mode: nil */
/* End: */