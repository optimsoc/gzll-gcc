/* Loop unrolling and peeling.
   Copyright (C) 2002-2013 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "rtl.h"
#include "tree.h"
#include "hard-reg-set.h"
#include "obstack.h"
#include "basic-block.h"
#include "cfgloop.h"
#include "params.h"
#include "expr.h"
#include "hash-table.h"
#include "recog.h"
#include "target.h"
#include "dumpfile.h"

/* This pass performs loop unrolling and peeling.  We only perform these
   optimizations on innermost loops (with single exception) because
   the impact on performance is greatest here, and we want to avoid
   unnecessary code size growth.  The gain is caused by greater sequentiality
   of code, better code to optimize for further passes and in some cases
   by fewer testings of exit conditions.  The main problem is code growth,
   that impacts performance negatively due to effect of caches.

   What we do:

   -- complete peeling of once-rolling loops; this is the above mentioned
      exception, as this causes loop to be cancelled completely and
      does not cause code growth
   -- complete peeling of loops that roll (small) constant times.
   -- simple peeling of first iterations of loops that do not roll much
      (according to profile feedback)
   -- unrolling of loops that roll constant times; this is almost always
      win, as we get rid of exit condition tests.
   -- unrolling of loops that roll number of times that we can compute
      in runtime; we also get rid of exit condition tests here, but there
      is the extra expense for calculating the number of iterations
   -- simple unrolling of remaining loops; this is performed only if we
      are asked to, as the gain is questionable in this case and often
      it may even slow down the code
   For more detailed descriptions of each of those, see comments at
   appropriate function below.

   There is a lot of parameters (defined and described in params.def) that
   control how much we unroll/peel.

   ??? A great problem is that we don't have a good way how to determine
   how many times we should unroll the loop; the experiments I have made
   showed that this choice may affect performance in order of several %.
   */

/* Information about induction variables to split.  */

struct iv_to_split
{
  rtx insn;		/* The insn in that the induction variable occurs.  */
  rtx orig_var;		/* The variable (register) for the IV before split.  */
  rtx base_var;		/* The variable on that the values in the further
			   iterations are based.  */
  rtx step;		/* Step of the induction variable.  */
  struct iv_to_split *next; /* Next entry in walking order.  */
  unsigned n_loc;
  unsigned loc[3];	/* Location where the definition of the induction
			   variable occurs in the insn.  For example if
			   N_LOC is 2, the expression is located at
			   XEXP (XEXP (single_set, loc[0]), loc[1]).  */
};

/* Information about accumulators to expand.  */

struct var_to_expand
{
  rtx insn;		           /* The insn in that the variable expansion occurs.  */
  rtx reg;                         /* The accumulator which is expanded.  */
  vec<rtx> var_expansions;   /* The copies of the accumulator which is expanded.  */
  struct var_to_expand *next;	   /* Next entry in walking order.  */
  enum rtx_code op;                /* The type of the accumulation - addition, subtraction
                                      or multiplication.  */
  int expansion_count;             /* Count the number of expansions generated so far.  */
  int reuse_expansion;             /* The expansion we intend to reuse to expand
                                      the accumulator.  If REUSE_EXPANSION is 0 reuse
                                      the original accumulator.  Else use
                                      var_expansions[REUSE_EXPANSION - 1].  */
};

/* Hashtable helper for iv_to_split.  */

struct iv_split_hasher : typed_free_remove <iv_to_split>
{
  typedef iv_to_split value_type;
  typedef iv_to_split compare_type;
  static inline hashval_t hash (const value_type *);
  static inline bool equal (const value_type *, const compare_type *);
};


/* A hash function for information about insns to split.  */

inline hashval_t
iv_split_hasher::hash (const value_type *ivts)
{
  return (hashval_t) INSN_UID (ivts->insn);
}

/* An equality functions for information about insns to split.  */

inline bool
iv_split_hasher::equal (const value_type *i1, const compare_type *i2)
{
  return i1->insn == i2->insn;
}

/* Hashtable helper for iv_to_split.  */

struct var_expand_hasher : typed_free_remove <var_to_expand>
{
  typedef var_to_expand value_type;
  typedef var_to_expand compare_type;
  static inline hashval_t hash (const value_type *);
  static inline bool equal (const value_type *, const compare_type *);
};

/* Return a hash for VES.  */

inline hashval_t
var_expand_hasher::hash (const value_type *ves)
{
  return (hashval_t) INSN_UID (ves->insn);
}

/* Return true if I1 and I2 refer to the same instruction.  */

inline bool
var_expand_hasher::equal (const value_type *i1, const compare_type *i2)
{
  return i1->insn == i2->insn;
}

/* Information about optimization applied in
   the unrolled loop.  */

struct opt_info
{
  hash_table <iv_split_hasher> insns_to_split; /* A hashtable of insns to
						  split.  */
  struct iv_to_split *iv_to_split_head; /* The first iv to split.  */
  struct iv_to_split **iv_to_split_tail; /* Pointer to the tail of the list.  */
  hash_table <var_expand_hasher> insns_with_var_to_expand; /* A hashtable of
					insns with accumulators to expand.  */
  struct var_to_expand *var_to_expand_head; /* The first var to expand.  */
  struct var_to_expand **var_to_expand_tail; /* Pointer to the tail of the list.  */
  unsigned first_new_block;        /* The first basic block that was
                                      duplicated.  */
  basic_block loop_exit;           /* The loop exit basic block.  */
  basic_block loop_preheader;      /* The loop preheader basic block.  */
};

static void decide_unrolling_and_peeling (int);
static void peel_loops_completely (int);
static void decide_peel_simple (struct loop *, int);
static void decide_peel_once_rolling (struct loop *, int);
static void decide_peel_completely (struct loop *, int);
static void decide_unroll_stupid (struct loop *, int);
static void decide_unroll_constant_iterations (struct loop *, int);
static void decide_unroll_runtime_iterations (struct loop *, int);
static void peel_loop_simple (struct loop *);
static void peel_loop_completely (struct loop *);
static void unroll_loop_stupid (struct loop *);
static void unroll_loop_constant_iterations (struct loop *);
static void unroll_loop_runtime_iterations (struct loop *);
static struct opt_info *analyze_insns_in_loop (struct loop *);
static void opt_info_start_duplication (struct opt_info *);
static void apply_opt_in_copies (struct opt_info *, unsigned, bool, bool);
static void free_opt_info (struct opt_info *);
static struct var_to_expand *analyze_insn_to_expand_var (struct loop*, rtx);
static bool referenced_in_one_insn_in_loop_p (struct loop *, rtx, int *);
static struct iv_to_split *analyze_iv_to_split_insn (rtx);
static void expand_var_during_unrolling (struct var_to_expand *, rtx);
static void insert_var_expansion_initialization (struct var_to_expand *,
						 basic_block);
static void combine_var_copies_in_loop_exit (struct var_to_expand *,
					     basic_block);
static rtx get_expansion (struct var_to_expand *);

/* Emit a message summarizing the unroll or peel that will be
   performed for LOOP, along with the loop's location LOCUS, if
   appropriate given the dump or -fopt-info settings.  */

static void
report_unroll_peel (struct loop *loop, location_t locus)
{
  struct niter_desc *desc;
  int niters = 0;
  int report_flags = MSG_OPTIMIZED_LOCATIONS | TDF_RTL | TDF_DETAILS;

  if (loop->lpt_decision.decision == LPT_NONE)
    return;

  if (!dump_enabled_p ())
    return;

  /* In the special case where the loop never iterated, emit
     a different message so that we don't report an unroll by 0.
     This matches the equivalent message emitted during tree unrolling.  */
  if (loop->lpt_decision.decision == LPT_PEEL_COMPLETELY
      && !loop->lpt_decision.times)
    {
      dump_printf_loc (report_flags, locus,
                       "loop turned into non-loop; it never loops.\n");
      return;
    }

  desc = get_simple_loop_desc (loop);

  if (desc->const_iter)
    niters = desc->niter;
  else if (loop->header->count)
    niters = expected_loop_iterations (loop);

  if (loop->lpt_decision.decision == LPT_PEEL_COMPLETELY)
    dump_printf_loc (report_flags, locus,
                     "loop with %d iterations completely unrolled",
		     loop->lpt_decision.times + 1);
  else
    dump_printf_loc (report_flags, locus,
                     "loop %s %d times",
                     (loop->lpt_decision.decision == LPT_PEEL_SIMPLE
                       ? "peeled" : "unrolled"),
                     loop->lpt_decision.times);
  if (profile_info)
    dump_printf (report_flags,
                 " (header execution count %d",
                 (int)loop->header->count);
  if (loop->lpt_decision.decision == LPT_PEEL_COMPLETELY)
    dump_printf (report_flags,
                 "%s%s iterations %d)",
                 profile_info ? ", " : " (",
                 desc->const_iter ? "const" : "average",
                 niters);
  else if (profile_info)
    dump_printf (report_flags, ")");

  dump_printf (report_flags, "\n");
}

/* Unroll and/or peel (depending on FLAGS) LOOPS.  */
void
unroll_and_peel_loops (int flags)
{
  struct loop *loop;
  bool changed = false;

  /* First perform complete loop peeling (it is almost surely a win,
     and affects parameters for further decision a lot).  */
  peel_loops_completely (flags);

  /* Now decide rest of unrolling and peeling.  */
  decide_unrolling_and_peeling (flags);

  /* Scan the loops, inner ones first.  */
  FOR_EACH_LOOP (loop, LI_FROM_INNERMOST)
    {
      /* And perform the appropriate transformations.  */
      switch (loop->lpt_decision.decision)
	{
	case LPT_PEEL_COMPLETELY:
	  /* Already done.  */
	  gcc_unreachable ();
	case LPT_PEEL_SIMPLE:
	  peel_loop_simple (loop);
	  changed = true;
	  break;
	case LPT_UNROLL_CONSTANT:
	  unroll_loop_constant_iterations (loop);
	  changed = true;
	  break;
	case LPT_UNROLL_RUNTIME:
	  unroll_loop_runtime_iterations (loop);
	  changed = true;
	  break;
	case LPT_UNROLL_STUPID:
	  unroll_loop_stupid (loop);
	  changed = true;
	  break;
	case LPT_NONE:
	  break;
	default:
	  gcc_unreachable ();
	}
    }

    if (changed)
      {
	calculate_dominance_info (CDI_DOMINATORS);
	fix_loop_structure (NULL);
      }

  iv_analysis_done ();
}

/* Check whether exit of the LOOP is at the end of loop body.  */

static bool
loop_exit_at_end_p (struct loop *loop)
{
  struct niter_desc *desc = get_simple_loop_desc (loop);
  rtx insn;

  if (desc->in_edge->dest != loop->latch)
    return false;

  /* Check that the latch is empty.  */
  FOR_BB_INSNS (loop->latch, insn)
    {
      if (NONDEBUG_INSN_P (insn))
	return false;
    }

  return true;
}

/* Depending on FLAGS, check whether to peel loops completely and do so.  */
static void
peel_loops_completely (int flags)
{
  struct loop *loop;
  bool changed = false;

  /* Scan the loops, the inner ones first.  */
  FOR_EACH_LOOP (loop, LI_FROM_INNERMOST)
    {
      loop->lpt_decision.decision = LPT_NONE;
      location_t locus = get_loop_location (loop);

      if (dump_enabled_p ())
	dump_printf_loc (TDF_RTL, locus,
                         ";; *** Considering loop %d at BB %d for "
                         "complete peeling ***\n",
                         loop->num, loop->header->index);

      loop->ninsns = num_loop_insns (loop);

      decide_peel_once_rolling (loop, flags);
      if (loop->lpt_decision.decision == LPT_NONE)
	decide_peel_completely (loop, flags);

      if (loop->lpt_decision.decision == LPT_PEEL_COMPLETELY)
	{
	  report_unroll_peel (loop, locus);
	  peel_loop_completely (loop);
	  changed = true;
	}
    }

    if (changed)
      {
	calculate_dominance_info (CDI_DOMINATORS);
	fix_loop_structure (NULL);
      }
}

/* Decide whether unroll or peel loops (depending on FLAGS) and how much.  */
static void
decide_unrolling_and_peeling (int flags)
{
  struct loop *loop;

  /* Scan the loops, inner ones first.  */
  FOR_EACH_LOOP (loop, LI_FROM_INNERMOST)
    {
      loop->lpt_decision.decision = LPT_NONE;
      location_t locus = get_loop_location (loop);

      if (dump_enabled_p ())
	dump_printf_loc (TDF_RTL, locus,
                         ";; *** Considering loop %d at BB %d for "
                         "unrolling and peeling ***\n",
                         loop->num, loop->header->index);

      /* Do not peel cold areas.  */
      if (optimize_loop_for_size_p (loop))
	{
	  if (dump_file)
	    fprintf (dump_file, ";; Not considering loop, cold area\n");
	  continue;
	}

      /* Can the loop be manipulated?  */
      if (!can_duplicate_loop_p (loop))
	{
	  if (dump_file)
	    fprintf (dump_file,
		     ";; Not considering loop, cannot duplicate\n");
	  continue;
	}

      /* Skip non-innermost loops.  */
      if (loop->inner)
	{
	  if (dump_file)
	    fprintf (dump_file, ";; Not considering loop, is not innermost\n");
	  continue;
	}

      loop->ninsns = num_loop_insns (loop);
      loop->av_ninsns = average_num_loop_insns (loop);

      /* Try transformations one by one in decreasing order of
	 priority.  */

      decide_unroll_constant_iterations (loop, flags);
      if (loop->lpt_decision.decision == LPT_NONE)
	decide_unroll_runtime_iterations (loop, flags);
      if (loop->lpt_decision.decision == LPT_NONE)
	decide_unroll_stupid (loop, flags);
      if (loop->lpt_decision.decision == LPT_NONE)
	decide_peel_simple (loop, flags);

      report_unroll_peel (loop, locus);
    }
}

/* Decide whether the LOOP is once rolling and suitable for complete
   peeling.  */
static void
decide_peel_once_rolling (struct loop *loop, int flags ATTRIBUTE_UNUSED)
{
  struct niter_desc *desc;

  if (dump_file)
    fprintf (dump_file, "\n;; Considering peeling once rolling loop\n");

  /* Is the loop small enough?  */
  if ((unsigned) PARAM_VALUE (PARAM_MAX_ONCE_PEELED_INSNS) < loop->ninsns)
    {
      if (dump_file)
	fprintf (dump_file, ";; Not considering loop, is too big\n");
      return;
    }

  /* Check for simple loops.  */
  desc = get_simple_loop_desc (loop);

  /* Check number of iterations.  */
  if (!desc->simple_p
      || desc->assumptions
      || desc->infinite
      || !desc->const_iter
      || (desc->niter != 0
	  && get_max_loop_iterations_int (loop) != 0))
    {
      if (dump_file)
	fprintf (dump_file,
		 ";; Unable to prove that the loop rolls exactly once\n");
      return;
    }

  /* Success.  */
  loop->lpt_decision.decision = LPT_PEEL_COMPLETELY;
}

/* Decide whether the LOOP is suitable for complete peeling.  */
static void
decide_peel_completely (struct loop *loop, int flags ATTRIBUTE_UNUSED)
{
  unsigned npeel;
  struct niter_desc *desc;

  if (dump_file)
    fprintf (dump_file, "\n;; Considering peeling completely\n");

  /* Skip non-innermost loops.  */
  if (loop->inner)
    {
      if (dump_file)
	fprintf (dump_file, ";; Not considering loop, is not innermost\n");
      return;
    }

  /* Do not peel cold areas.  */
  if (optimize_loop_for_size_p (loop))
    {
      if (dump_file)
	fprintf (dump_file, ";; Not considering loop, cold area\n");
      return;
    }

  /* Can the loop be manipulated?  */
  if (!can_duplicate_loop_p (loop))
    {
      if (dump_file)
	fprintf (dump_file,
		 ";; Not considering loop, cannot duplicate\n");
      return;
    }

  /* npeel = number of iterations to peel.  */
  npeel = PARAM_VALUE (PARAM_MAX_COMPLETELY_PEELED_INSNS) / loop->ninsns;
  if (npeel > (unsigned) PARAM_VALUE (PARAM_MAX_COMPLETELY_PEEL_TIMES))
    npeel = PARAM_VALUE (PARAM_MAX_COMPLETELY_PEEL_TIMES);

  /* Is the loop small enough?  */
  if (!npeel)
    {
      if (dump_file)
	fprintf (dump_file, ";; Not considering loop, is too big\n");
      return;
    }

  /* Check for simple loops.  */
  desc = get_simple_loop_desc (loop);

  /* Check number of iterations.  */
  if (!desc->simple_p
      || desc->assumptions
      || !desc->const_iter
      || desc->infinite)
    {
      if (dump_file)
	fprintf (dump_file,
		 ";; Unable to prove that the loop iterates constant times\n");
      return;
    }

  if (desc->niter > npeel - 1)
    {
      if (dump_file)
	{
	  fprintf (dump_file,
		   ";; Not peeling loop completely, rolls too much (");
	  fprintf (dump_file, HOST_WIDEST_INT_PRINT_DEC, desc->niter);
	  fprintf (dump_file, " iterations > %d [maximum peelings])\n", npeel);
	}
      return;
    }

  /* Success.  */
  loop->lpt_decision.decision = LPT_PEEL_COMPLETELY;
}

/* Peel all iterations of LOOP, remove exit edges and cancel the loop
   completely.  The transformation done:

   for (i = 0; i < 4; i++)
     body;

   ==>

   i = 0;
   body; i++;
   body; i++;
   body; i++;
   body; i++;
   */
static void
peel_loop_completely (struct loop *loop)
{
  sbitmap wont_exit;
  unsigned HOST_WIDE_INT npeel;
  unsigned i;
  edge ein;
  struct niter_desc *desc = get_simple_loop_desc (loop);
  struct opt_info *opt_info = NULL;

  npeel = desc->niter;

  if (npeel)
    {
      bool ok;

      wont_exit = sbitmap_alloc (npeel + 1);
      bitmap_ones (wont_exit);
      bitmap_clear_bit (wont_exit, 0);
      if (desc->noloop_assumptions)
	bitmap_clear_bit (wont_exit, 1);

      auto_vec<edge> remove_edges;
      if (flag_split_ivs_in_unroller)
        opt_info = analyze_insns_in_loop (loop);

      opt_info_start_duplication (opt_info);
      ok = duplicate_loop_to_header_edge (loop, loop_preheader_edge (loop),
					  npeel,
					  wont_exit, desc->out_edge,
					  &remove_edges,
					  DLTHE_FLAG_UPDATE_FREQ
					  | DLTHE_FLAG_COMPLETTE_PEEL
					  | (opt_info
					     ? DLTHE_RECORD_COPY_NUMBER : 0));
      gcc_assert (ok);

      free (wont_exit);

      if (opt_info)
 	{
 	  apply_opt_in_copies (opt_info, npeel, false, true);
 	  free_opt_info (opt_info);
 	}

      /* Remove the exit edges.  */
      FOR_EACH_VEC_ELT (remove_edges, i, ein)
	remove_path (ein);
    }

  ein = desc->in_edge;
  free_simple_loop_desc (loop);

  /* Now remove the unreachable part of the last iteration and cancel
     the loop.  */
  remove_path (ein);

  if (dump_file)
    fprintf (dump_file, ";; Peeled loop completely, %d times\n", (int) npeel);
}

/* Decide whether to unroll LOOP iterating constant number of times
   and how much.  */

static void
decide_unroll_constant_iterations (struct loop *loop, int flags)
{
  unsigned nunroll, nunroll_by_av, best_copies, best_unroll = 0, n_copies, i;
  struct niter_desc *desc;
  double_int iterations;

  if (!(flags & UAP_UNROLL))
    {
      /* We were not asked to, just return back silently.  */
      return;
    }

  if (dump_file)
    fprintf (dump_file,
	     "\n;; Considering unrolling loop with constant "
	     "number of iterations\n");

  /* nunroll = total number of copies of the original loop body in
     unrolled loop (i.e. if it is 2, we have to duplicate loop body once.  */
  nunroll = PARAM_VALUE (PARAM_MAX_UNROLLED_INSNS) / loop->ninsns;
  nunroll_by_av
    = PARAM_VALUE (PARAM_MAX_AVERAGE_UNROLLED_INSNS) / loop->av_ninsns;
  if (nunroll > nunroll_by_av)
    nunroll = nunroll_by_av;
  if (nunroll > (unsigned) PARAM_VALUE (PARAM_MAX_UNROLL_TIMES))
    nunroll = PARAM_VALUE (PARAM_MAX_UNROLL_TIMES);

  if (targetm.loop_unroll_adjust)
    nunroll = targetm.loop_unroll_adjust (nunroll, loop);

  /* Skip big loops.  */
  if (nunroll <= 1)
    {
      if (dump_file)
	fprintf (dump_file, ";; Not considering loop, is too big\n");
      return;
    }

  /* Check for simple loops.  */
  desc = get_simple_loop_desc (loop);

  /* Check number of iterations.  */
  if (!desc->simple_p || !desc->const_iter || desc->assumptions)
    {
      if (dump_file)
	fprintf (dump_file,
		 ";; Unable to prove that the loop iterates constant times\n");
      return;
    }

  /* Check whether the loop rolls enough to consider.  
     Consult also loop bounds and profile; in the case the loop has more
     than one exit it may well loop less than determined maximal number
     of iterations.  */
  if (desc->niter < 2 * nunroll
      || ((get_estimated_loop_iterations (loop, &iterations)
	   || get_max_loop_iterations (loop, &iterations))
	  && iterations.ult (double_int::from_shwi (2 * nunroll))))
    {
      if (dump_file)
	fprintf (dump_file, ";; Not unrolling loop, doesn't roll\n");
      return;
    }

  /* Success; now compute number of iterations to unroll.  We alter
     nunroll so that as few as possible copies of loop body are
     necessary, while still not decreasing the number of unrollings
     too much (at most by 1).  */
  best_copies = 2 * nunroll + 10;

  i = 2 * nunroll + 2;
  if (i - 1 >= desc->niter)
    i = desc->niter - 2;

  for (; i >= nunroll - 1; i--)
    {
      unsigned exit_mod = desc->niter % (i + 1);

      if (!loop_exit_at_end_p (loop))
	n_copies = exit_mod + i + 1;
      else if (exit_mod != (unsigned) i
	       || desc->noloop_assumptions != NULL_RTX)
	n_copies = exit_mod + i + 2;
      else
	n_copies = i + 1;

      if (n_copies < best_copies)
	{
	  best_copies = n_copies;
	  best_unroll = i;
	}
    }

  loop->lpt_decision.decision = LPT_UNROLL_CONSTANT;
  loop->lpt_decision.times = best_unroll;
}

/* Unroll LOOP with constant number of iterations LOOP->LPT_DECISION.TIMES times.
   The transformation does this:

   for (i = 0; i < 102; i++)
     body;

   ==>  (LOOP->LPT_DECISION.TIMES == 3)

   i = 0;
   body; i++;
   body; i++;
   while (i < 102)
     {
       body; i++;
       body; i++;
       body; i++;
       body; i++;
     }
  */
static void
unroll_loop_constant_iterations (struct loop *loop)
{
  unsigned HOST_WIDE_INT niter;
  unsigned exit_mod;
  sbitmap wont_exit;
  unsigned i;
  edge e;
  unsigned max_unroll = loop->lpt_decision.times;
  struct niter_desc *desc = get_simple_loop_desc (loop);
  bool exit_at_end = loop_exit_at_end_p (loop);
  struct opt_info *opt_info = NULL;
  bool ok;

  niter = desc->niter;

  /* Should not get here (such loop should be peeled instead).  */
  gcc_assert (niter > max_unroll + 1);

  exit_mod = niter % (max_unroll + 1);

  wont_exit = sbitmap_alloc (max_unroll + 1);
  bitmap_ones (wont_exit);

  auto_vec<edge> remove_edges;
  if (flag_split_ivs_in_unroller
      || flag_variable_expansion_in_unroller)
    opt_info = analyze_insns_in_loop (loop);

  if (!exit_at_end)
    {
      /* The exit is not at the end of the loop; leave exit test
	 in the first copy, so that the loops that start with test
	 of exit condition have continuous body after unrolling.  */

      if (dump_file)
	fprintf (dump_file, ";; Condition at beginning of loop.\n");

      /* Peel exit_mod iterations.  */
      bitmap_clear_bit (wont_exit, 0);
      if (desc->noloop_assumptions)
	bitmap_clear_bit (wont_exit, 1);

      if (exit_mod)
	{
	  opt_info_start_duplication (opt_info);
          ok = duplicate_loop_to_header_edge (loop, loop_preheader_edge (loop),
					      exit_mod,
					      wont_exit, desc->out_edge,
					      &remove_edges,
					      DLTHE_FLAG_UPDATE_FREQ
					      | (opt_info && exit_mod > 1
						 ? DLTHE_RECORD_COPY_NUMBER
						   : 0));
	  gcc_assert (ok);

          if (opt_info && exit_mod > 1)
 	    apply_opt_in_copies (opt_info, exit_mod, false, false);

	  desc->noloop_assumptions = NULL_RTX;
	  desc->niter -= exit_mod;
	  loop->nb_iterations_upper_bound -= double_int::from_uhwi (exit_mod);
	  if (loop->any_estimate
	      && double_int::from_uhwi (exit_mod).ule
	           (loop->nb_iterations_estimate))
	    loop->nb_iterations_estimate -= double_int::from_uhwi (exit_mod);
	  else
	    loop->any_estimate = false;
	}

      bitmap_set_bit (wont_exit, 1);
    }
  else
    {
      /* Leave exit test in last copy, for the same reason as above if
	 the loop tests the condition at the end of loop body.  */

      if (dump_file)
	fprintf (dump_file, ";; Condition at end of loop.\n");

      /* We know that niter >= max_unroll + 2; so we do not need to care of
	 case when we would exit before reaching the loop.  So just peel
	 exit_mod + 1 iterations.  */
      if (exit_mod != max_unroll
	  || desc->noloop_assumptions)
	{
	  bitmap_clear_bit (wont_exit, 0);
	  if (desc->noloop_assumptions)
	    bitmap_clear_bit (wont_exit, 1);

          opt_info_start_duplication (opt_info);
	  ok = duplicate_loop_to_header_edge (loop, loop_preheader_edge (loop),
					      exit_mod + 1,
					      wont_exit, desc->out_edge,
					      &remove_edges,
					      DLTHE_FLAG_UPDATE_FREQ
					      | (opt_info && exit_mod > 0
						 ? DLTHE_RECORD_COPY_NUMBER
						   : 0));
	  gcc_assert (ok);

          if (opt_info && exit_mod > 0)
  	    apply_opt_in_copies (opt_info, exit_mod + 1, false, false);

	  desc->niter -= exit_mod + 1;
	  loop->nb_iterations_upper_bound -= double_int::from_uhwi (exit_mod + 1);
	  if (loop->any_estimate
	      && double_int::from_uhwi (exit_mod + 1).ule
	           (loop->nb_iterations_estimate))
	    loop->nb_iterations_estimate -= double_int::from_uhwi (exit_mod + 1);
	  else
	    loop->any_estimate = false;
	  desc->noloop_assumptions = NULL_RTX;

	  bitmap_set_bit (wont_exit, 0);
	  bitmap_set_bit (wont_exit, 1);
	}

      bitmap_clear_bit (wont_exit, max_unroll);
    }

  /* Now unroll the loop.  */

  opt_info_start_duplication (opt_info);
  ok = duplicate_loop_to_header_edge (loop, loop_latch_edge (loop),
				      max_unroll,
				      wont_exit, desc->out_edge,
				      &remove_edges,
				      DLTHE_FLAG_UPDATE_FREQ
				      | (opt_info
					 ? DLTHE_RECORD_COPY_NUMBER
					   : 0));
  gcc_assert (ok);

  if (opt_info)
    {
      apply_opt_in_copies (opt_info, max_unroll, true, true);
      free_opt_info (opt_info);
    }

  free (wont_exit);

  if (exit_at_end)
    {
      basic_block exit_block = get_bb_copy (desc->in_edge->src);
      /* Find a new in and out edge; they are in the last copy we have made.  */

      if (EDGE_SUCC (exit_block, 0)->dest == desc->out_edge->dest)
	{
	  desc->out_edge = EDGE_SUCC (exit_block, 0);
	  desc->in_edge = EDGE_SUCC (exit_block, 1);
	}
      else
	{
	  desc->out_edge = EDGE_SUCC (exit_block, 1);
	  desc->in_edge = EDGE_SUCC (exit_block, 0);
	}
    }

  desc->niter /= max_unroll + 1;
  loop->nb_iterations_upper_bound
    = loop->nb_iterations_upper_bound.udiv (double_int::from_uhwi (max_unroll
								   + 1),
					    TRUNC_DIV_EXPR);
  if (loop->any_estimate)
    loop->nb_iterations_estimate
      = loop->nb_iterations_estimate.udiv (double_int::from_uhwi (max_unroll
							          + 1),
				           TRUNC_DIV_EXPR);
  desc->niter_expr = GEN_INT (desc->niter);

  /* Remove the edges.  */
  FOR_EACH_VEC_ELT (remove_edges, i, e)
    remove_path (e);

  if (dump_file)
    fprintf (dump_file,
	     ";; Unrolled loop %d times, constant # of iterations %i insns\n",
	     max_unroll, num_loop_insns (loop));
}

/* Decide whether to unroll LOOP iterating runtime computable number of times
   and how much.  */
static void
decide_unroll_runtime_iterations (struct loop *loop, int flags)
{
  unsigned nunroll, nunroll_by_av, i;
  struct niter_desc *desc;
  double_int iterations;

  if (!(flags & UAP_UNROLL))
    {
      /* We were not asked to, just return back silently.  */
      return;
    }

  if (dump_file)
    fprintf (dump_file,
	     "\n;; Considering unrolling loop with runtime "
	     "computable number of iterations\n");

  /* nunroll = total number of copies of the original loop body in
     unrolled loop (i.e. if it is 2, we have to duplicate loop body once.  */
  nunroll = PARAM_VALUE (PARAM_MAX_UNROLLED_INSNS) / loop->ninsns;
  nunroll_by_av = PARAM_VALUE (PARAM_MAX_AVERAGE_UNROLLED_INSNS) / loop->av_ninsns;
  if (nunroll > nunroll_by_av)
    nunroll = nunroll_by_av;
  if (nunroll > (unsigned) PARAM_VALUE (PARAM_MAX_UNROLL_TIMES))
    nunroll = PARAM_VALUE (PARAM_MAX_UNROLL_TIMES);

  if (targetm.loop_unroll_adjust)
    nunroll = targetm.loop_unroll_adjust (nunroll, loop);

  /* Skip big loops.  */
  if (nunroll <= 1)
    {
      if (dump_file)
	fprintf (dump_file, ";; Not considering loop, is too big\n");
      return;
    }

  /* Check for simple loops.  */
  desc = get_simple_loop_desc (loop);

  /* Check simpleness.  */
  if (!desc->simple_p || desc->assumptions)
    {
      if (dump_file)
	fprintf (dump_file,
		 ";; Unable to prove that the number of iterations "
		 "can be counted in runtime\n");
      return;
    }

  if (desc->const_iter)
    {
      if (dump_file)
	fprintf (dump_file, ";; Loop iterates constant times\n");
      return;
    }

  /* Check whether the loop rolls.  */
  if ((get_estimated_loop_iterations (loop, &iterations)
       || get_max_loop_iterations (loop, &iterations))
      && iterations.ult (double_int::from_shwi (2 * nunroll)))
    {
      if (dump_file)
	fprintf (dump_file, ";; Not unrolling loop, doesn't roll\n");
      return;
    }

  /* Success; now force nunroll to be power of 2, as we are unable to
     cope with overflows in computation of number of iterations.  */
  for (i = 1; 2 * i <= nunroll; i *= 2)
    continue;

  loop->lpt_decision.decision = LPT_UNROLL_RUNTIME;
  loop->lpt_decision.times = i - 1;
}

/* Splits edge E and inserts the sequence of instructions INSNS on it, and
   returns the newly created block.  If INSNS is NULL_RTX, nothing is changed
   and NULL is returned instead.  */

basic_block
split_edge_and_insert (edge e, rtx insns)
{
  basic_block bb;

  if (!insns)
    return NULL;
  bb = split_edge (e);
  emit_insn_after (insns, BB_END (bb));

  /* ??? We used to assume that INSNS can contain control flow insns, and
     that we had to try to find sub basic blocks in BB to maintain a valid
     CFG.  For this purpose we used to set the BB_SUPERBLOCK flag on BB
     and call break_superblocks when going out of cfglayout mode.  But it
     turns out that this never happens; and that if it does ever happen,
     the TODO_verify_flow at the end of the RTL loop passes would fail.

     There are two reasons why we expected we could have control flow insns
     in INSNS.  The first is when a comparison has to be done in parts, and
     the second is when the number of iterations is computed for loops with
     the number of iterations known at runtime.  In both cases, test cases
     to get control flow in INSNS appear to be impossible to construct:

      * If do_compare_rtx_and_jump needs several branches to do comparison
	in a mode that needs comparison by parts, we cannot analyze the
	number of iterations of the loop, and we never get to unrolling it.

      * The code in expand_divmod that was suspected to cause creation of
	branching code seems to be only accessed for signed division.  The
	divisions used by # of iterations analysis are always unsigned.
	Problems might arise on architectures that emits branching code
	for some operations that may appear in the unroller (especially
	for division), but we have no such architectures.

     Considering all this, it was decided that we should for now assume
     that INSNS can in theory contain control flow insns, but in practice
     it never does.  So we don't handle the theoretical case, and should
     a real failure ever show up, we have a pretty good clue for how to
     fix it.  */

  return bb;
}

/* Unroll LOOP for which we are able to count number of iterations in runtime
   LOOP->LPT_DECISION.TIMES times.  The transformation does this (with some
   extra care for case n < 0):

   for (i = 0; i < n; i++)
     body;

   ==>  (LOOP->LPT_DECISION.TIMES == 3)

   i = 0;
   mod = n % 4;

   switch (mod)
     {
       case 3:
         body; i++;
       case 2:
         body; i++;
       case 1:
         body; i++;
       case 0: ;
     }

   while (i < n)
     {
       body; i++;
       body; i++;
       body; i++;
       body; i++;
     }
   */
static void
unroll_loop_runtime_iterations (struct loop *loop)
{
  rtx old_niter, niter, init_code, branch_code, tmp;
  unsigned i, j, p;
  basic_block preheader, *body, swtch, ezc_swtch;
  sbitmap wont_exit;
  int may_exit_copy;
  unsigned n_peel;
  edge e;
  bool extra_zero_check, last_may_exit;
  unsigned max_unroll = loop->lpt_decision.times;
  struct niter_desc *desc = get_simple_loop_desc (loop);
  bool exit_at_end = loop_exit_at_end_p (loop);
  struct opt_info *opt_info = NULL;
  bool ok;

  if (flag_split_ivs_in_unroller
      || flag_variable_expansion_in_unroller)
    opt_info = analyze_insns_in_loop (loop);

  /* Remember blocks whose dominators will have to be updated.  */
  auto_vec<basic_block> dom_bbs;

  body = get_loop_body (loop);
  for (i = 0; i < loop->num_nodes; i++)
    {
      vec<basic_block> ldom;
      basic_block bb;

      ldom = get_dominated_by (CDI_DOMINATORS, body[i]);
      FOR_EACH_VEC_ELT (ldom, j, bb)
	if (!flow_bb_inside_loop_p (loop, bb))
	  dom_bbs.safe_push (bb);

      ldom.release ();
    }
  free (body);

  if (!exit_at_end)
    {
      /* Leave exit in first copy (for explanation why see comment in
	 unroll_loop_constant_iterations).  */
      may_exit_copy = 0;
      n_peel = max_unroll - 1;
      extra_zero_check = true;
      last_may_exit = false;
    }
  else
    {
      /* Leave exit in last copy (for explanation why see comment in
	 unroll_loop_constant_iterations).  */
      may_exit_copy = max_unroll;
      n_peel = max_unroll;
      extra_zero_check = false;
      last_may_exit = true;
    }

  /* Get expression for number of iterations.  */
  start_sequence ();
  old_niter = niter = gen_reg_rtx (desc->mode);
  tmp = force_operand (copy_rtx (desc->niter_expr), niter);
  if (tmp != niter)
    emit_move_insn (niter, tmp);

  /* Count modulo by ANDing it with max_unroll; we use the fact that
     the number of unrollings is a power of two, and thus this is correct
     even if there is overflow in the computation.  */
  niter = expand_simple_binop (desc->mode, AND,
			       niter, gen_int_mode (max_unroll, desc->mode),
			       NULL_RTX, 0, OPTAB_LIB_WIDEN);

  init_code = get_insns ();
  end_sequence ();
  unshare_all_rtl_in_chain (init_code);

  /* Precondition the loop.  */
  split_edge_and_insert (loop_preheader_edge (loop), init_code);

  auto_vec<edge> remove_edges;

  wont_exit = sbitmap_alloc (max_unroll + 2);

  /* Peel the first copy of loop body (almost always we must leave exit test
     here; the only exception is when we have extra zero check and the number
     of iterations is reliable.  Also record the place of (possible) extra
     zero check.  */
  bitmap_clear (wont_exit);
  if (extra_zero_check
      && !desc->noloop_assumptions)
    bitmap_set_bit (wont_exit, 1);
  ezc_swtch = loop_preheader_edge (loop)->src;
  ok = duplicate_loop_to_header_edge (loop, loop_preheader_edge (loop),
				      1, wont_exit, desc->out_edge,
				      &remove_edges,
				      DLTHE_FLAG_UPDATE_FREQ);
  gcc_assert (ok);

  /* Record the place where switch will be built for preconditioning.  */
  swtch = split_edge (loop_preheader_edge (loop));

  for (i = 0; i < n_peel; i++)
    {
      /* Peel the copy.  */
      bitmap_clear (wont_exit);
      if (i != n_peel - 1 || !last_may_exit)
	bitmap_set_bit (wont_exit, 1);
      ok = duplicate_loop_to_header_edge (loop, loop_preheader_edge (loop),
					  1, wont_exit, desc->out_edge,
					  &remove_edges,
					  DLTHE_FLAG_UPDATE_FREQ);
      gcc_assert (ok);

      /* Create item for switch.  */
      j = n_peel - i - (extra_zero_check ? 0 : 1);
      p = REG_BR_PROB_BASE / (i + 2);

      preheader = split_edge (loop_preheader_edge (loop));
      branch_code = compare_and_jump_seq (copy_rtx (niter), GEN_INT (j), EQ,
					  block_label (preheader), p,
					  NULL_RTX);

      /* We rely on the fact that the compare and jump cannot be optimized out,
	 and hence the cfg we create is correct.  */
      gcc_assert (branch_code != NULL_RTX);

      swtch = split_edge_and_insert (single_pred_edge (swtch), branch_code);
      set_immediate_dominator (CDI_DOMINATORS, preheader, swtch);
      single_pred_edge (swtch)->probability = REG_BR_PROB_BASE - p;
      e = make_edge (swtch, preheader,
		     single_succ_edge (swtch)->flags & EDGE_IRREDUCIBLE_LOOP);
      e->count = RDIV (preheader->count * REG_BR_PROB_BASE, p);
      e->probability = p;
    }

  if (extra_zero_check)
    {
      /* Add branch for zero iterations.  */
      p = REG_BR_PROB_BASE / (max_unroll + 1);
      swtch = ezc_swtch;
      preheader = split_edge (loop_preheader_edge (loop));
      branch_code = compare_and_jump_seq (copy_rtx (niter), const0_rtx, EQ,
					  block_label (preheader), p,
					  NULL_RTX);
      gcc_assert (branch_code != NULL_RTX);

      swtch = split_edge_and_insert (single_succ_edge (swtch), branch_code);
      set_immediate_dominator (CDI_DOMINATORS, preheader, swtch);
      single_succ_edge (swtch)->probability = REG_BR_PROB_BASE - p;
      e = make_edge (swtch, preheader,
		     single_succ_edge (swtch)->flags & EDGE_IRREDUCIBLE_LOOP);
      e->count = RDIV (preheader->count * REG_BR_PROB_BASE, p);
      e->probability = p;
    }

  /* Recount dominators for outer blocks.  */
  iterate_fix_dominators (CDI_DOMINATORS, dom_bbs, false);

  /* And unroll loop.  */

  bitmap_ones (wont_exit);
  bitmap_clear_bit (wont_exit, may_exit_copy);
  opt_info_start_duplication (opt_info);

  ok = duplicate_loop_to_header_edge (loop, loop_latch_edge (loop),
				      max_unroll,
				      wont_exit, desc->out_edge,
				      &remove_edges,
				      DLTHE_FLAG_UPDATE_FREQ
				      | (opt_info
					 ? DLTHE_RECORD_COPY_NUMBER
					   : 0));
  gcc_assert (ok);

  if (opt_info)
    {
      apply_opt_in_copies (opt_info, max_unroll, true, true);
      free_opt_info (opt_info);
    }

  free (wont_exit);

  if (exit_at_end)
    {
      basic_block exit_block = get_bb_copy (desc->in_edge->src);
      /* Find a new in and out edge; they are in the last copy we have
	 made.  */

      if (EDGE_SUCC (exit_block, 0)->dest == desc->out_edge->dest)
	{
	  desc->out_edge = EDGE_SUCC (exit_block, 0);
	  desc->in_edge = EDGE_SUCC (exit_block, 1);
	}
      else
	{
	  desc->out_edge = EDGE_SUCC (exit_block, 1);
	  desc->in_edge = EDGE_SUCC (exit_block, 0);
	}
    }

  /* Remove the edges.  */
  FOR_EACH_VEC_ELT (remove_edges, i, e)
    remove_path (e);

  /* We must be careful when updating the number of iterations due to
     preconditioning and the fact that the value must be valid at entry
     of the loop.  After passing through the above code, we see that
     the correct new number of iterations is this:  */
  gcc_assert (!desc->const_iter);
  desc->niter_expr =
    simplify_gen_binary (UDIV, desc->mode, old_niter,
			 gen_int_mode (max_unroll + 1, desc->mode));
  loop->nb_iterations_upper_bound
    = loop->nb_iterations_upper_bound.udiv (double_int::from_uhwi (max_unroll
								   + 1),
					    TRUNC_DIV_EXPR);
  if (loop->any_estimate)
    loop->nb_iterations_estimate
      = loop->nb_iterations_estimate.udiv (double_int::from_uhwi (max_unroll
							          + 1),
				           TRUNC_DIV_EXPR);
  if (exit_at_end)
    {
      desc->niter_expr =
	simplify_gen_binary (MINUS, desc->mode, desc->niter_expr, const1_rtx);
      desc->noloop_assumptions = NULL_RTX;
      --loop->nb_iterations_upper_bound;
      if (loop->any_estimate
	  && loop->nb_iterations_estimate != double_int_zero)
	--loop->nb_iterations_estimate;
      else
	loop->any_estimate = false;
    }

  if (dump_file)
    fprintf (dump_file,
	     ";; Unrolled loop %d times, counting # of iterations "
	     "in runtime, %i insns\n",
	     max_unroll, num_loop_insns (loop));
}

/* Decide whether to simply peel LOOP and how much.  */
static void
decide_peel_simple (struct loop *loop, int flags)
{
  unsigned npeel;
  double_int iterations;

  if (!(flags & UAP_PEEL))
    {
      /* We were not asked to, just return back silently.  */
      return;
    }

  if (dump_file)
    fprintf (dump_file, "\n;; Considering simply peeling loop\n");

  /* npeel = number of iterations to peel.  */
  npeel = PARAM_VALUE (PARAM_MAX_PEELED_INSNS) / loop->ninsns;
  if (npeel > (unsigned) PARAM_VALUE (PARAM_MAX_PEEL_TIMES))
    npeel = PARAM_VALUE (PARAM_MAX_PEEL_TIMES);

  /* Skip big loops.  */
  if (!npeel)
    {
      if (dump_file)
	fprintf (dump_file, ";; Not considering loop, is too big\n");
      return;
    }

  /* Do not simply peel loops with branches inside -- it increases number
     of mispredicts.  
     Exception is when we do have profile and we however have good chance
     to peel proper number of iterations loop will iterate in practice.
     TODO: this heuristic needs tunning; while for complette unrolling
     the branch inside loop mostly eliminates any improvements, for
     peeling it is not the case.  Also a function call inside loop is
     also branch from branch prediction POV (and probably better reason
     to not unroll/peel).  */
  if (num_loop_branches (loop) > 1
      && profile_status != PROFILE_READ)
    {
      if (dump_file)
	fprintf (dump_file, ";; Not peeling, contains branches\n");
      return;
    }

  /* If we have realistic estimate on number of iterations, use it.  */
  if (get_estimated_loop_iterations (loop, &iterations))
    {
      if (double_int::from_shwi (npeel).ule (iterations))
	{
	  if (dump_file)
	    {
	      fprintf (dump_file, ";; Not peeling loop, rolls too much (");
	      fprintf (dump_file, HOST_WIDEST_INT_PRINT_DEC,
		       (HOST_WIDEST_INT) (iterations.to_shwi () + 1));
	      fprintf (dump_file, " iterations > %d [maximum peelings])\n",
		       npeel);
	    }
	  return;
	}
      npeel = iterations.to_shwi () + 1;
    }
  /* If we have small enough bound on iterations, we can still peel (completely
     unroll).  */
  else if (get_max_loop_iterations (loop, &iterations)
           && iterations.ult (double_int::from_shwi (npeel)))
    npeel = iterations.to_shwi () + 1;
  else
    {
      /* For now we have no good heuristics to decide whether loop peeling
         will be effective, so disable it.  */
      if (dump_file)
	fprintf (dump_file,
		 ";; Not peeling loop, no evidence it will be profitable\n");
      return;
    }

  /* Success.  */
  loop->lpt_decision.decision = LPT_PEEL_SIMPLE;
  loop->lpt_decision.times = npeel;
}

/* Peel a LOOP LOOP->LPT_DECISION.TIMES times.  The transformation does this:

   while (cond)
     body;

   ==>  (LOOP->LPT_DECISION.TIMES == 3)

   if (!cond) goto end;
   body;
   if (!cond) goto end;
   body;
   if (!cond) goto end;
   body;
   while (cond)
     body;
   end: ;
   */
static void
peel_loop_simple (struct loop *loop)
{
  sbitmap wont_exit;
  unsigned npeel = loop->lpt_decision.times;
  struct niter_desc *desc = get_simple_loop_desc (loop);
  struct opt_info *opt_info = NULL;
  bool ok;

  if (flag_split_ivs_in_unroller && npeel > 1)
    opt_info = analyze_insns_in_loop (loop);

  wont_exit = sbitmap_alloc (npeel + 1);
  bitmap_clear (wont_exit);

  opt_info_start_duplication (opt_info);

  ok = duplicate_loop_to_header_edge (loop, loop_preheader_edge (loop),
				      npeel, wont_exit, NULL,
				      NULL, DLTHE_FLAG_UPDATE_FREQ
				      | (opt_info
					 ? DLTHE_RECORD_COPY_NUMBER
					   : 0));
  gcc_assert (ok);

  free (wont_exit);

  if (opt_info)
    {
      apply_opt_in_copies (opt_info, npeel, false, false);
      free_opt_info (opt_info);
    }

  if (desc->simple_p)
    {
      if (desc->const_iter)
	{
	  desc->niter -= npeel;
	  desc->niter_expr = GEN_INT (desc->niter);
	  desc->noloop_assumptions = NULL_RTX;
	}
      else
	{
	  /* We cannot just update niter_expr, as its value might be clobbered
	     inside loop.  We could handle this by counting the number into
	     temporary just like we do in runtime unrolling, but it does not
	     seem worthwhile.  */
	  free_simple_loop_desc (loop);
	}
    }
  if (dump_file)
    fprintf (dump_file, ";; Peeling loop %d times\n", npeel);
}

/* Decide whether to unroll LOOP stupidly and how much.  */
static void
decide_unroll_stupid (struct loop *loop, int flags)
{
  unsigned nunroll, nunroll_by_av, i;
  struct niter_desc *desc;
  double_int iterations;

  if (!(flags & UAP_UNROLL_ALL))
    {
      /* We were not asked to, just return back silently.  */
      return;
    }

  if (dump_file)
    fprintf (dump_file, "\n;; Considering unrolling loop stupidly\n");

  /* nunroll = total number of copies of the original loop body in
     unrolled loop (i.e. if it is 2, we have to duplicate loop body once.  */
  nunroll = PARAM_VALUE (PARAM_MAX_UNROLLED_INSNS) / loop->ninsns;
  nunroll_by_av
    = PARAM_VALUE (PARAM_MAX_AVERAGE_UNROLLED_INSNS) / loop->av_ninsns;
  if (nunroll > nunroll_by_av)
    nunroll = nunroll_by_av;
  if (nunroll > (unsigned) PARAM_VALUE (PARAM_MAX_UNROLL_TIMES))
    nunroll = PARAM_VALUE (PARAM_MAX_UNROLL_TIMES);

  if (targetm.loop_unroll_adjust)
    nunroll = targetm.loop_unroll_adjust (nunroll, loop);

  /* Skip big loops.  */
  if (nunroll <= 1)
    {
      if (dump_file)
	fprintf (dump_file, ";; Not considering loop, is too big\n");
      return;
    }

  /* Check for simple loops.  */
  desc = get_simple_loop_desc (loop);

  /* Check simpleness.  */
  if (desc->simple_p && !desc->assumptions)
    {
      if (dump_file)
	fprintf (dump_file, ";; The loop is simple\n");
      return;
    }

  /* Do not unroll loops with branches inside -- it increases number
     of mispredicts. 
     TODO: this heuristic needs tunning; call inside the loop body
     is also relatively good reason to not unroll.  */
  if (num_loop_branches (loop) > 1)
    {
      if (dump_file)
	fprintf (dump_file, ";; Not unrolling, contains branches\n");
      return;
    }

  /* Check whether the loop rolls.  */
  if ((get_estimated_loop_iterations (loop, &iterations)
       || get_max_loop_iterations (loop, &iterations))
      && iterations.ult (double_int::from_shwi (2 * nunroll)))
    {
      if (dump_file)
	fprintf (dump_file, ";; Not unrolling loop, doesn't roll\n");
      return;
    }

  /* Success.  Now force nunroll to be power of 2, as it seems that this
     improves results (partially because of better alignments, partially
     because of some dark magic).  */
  for (i = 1; 2 * i <= nunroll; i *= 2)
    continue;

  loop->lpt_decision.decision = LPT_UNROLL_STUPID;
  loop->lpt_decision.times = i - 1;
}

/* Unroll a LOOP LOOP->LPT_DECISION.TIMES times.  The transformation does this:

   while (cond)
     body;

   ==>  (LOOP->LPT_DECISION.TIMES == 3)

   while (cond)
     {
       body;
       if (!cond) break;
       body;
       if (!cond) break;
       body;
       if (!cond) break;
       body;
     }
   */
static void
unroll_loop_stupid (struct loop *loop)
{
  sbitmap wont_exit;
  unsigned nunroll = loop->lpt_decision.times;
  struct niter_desc *desc = get_simple_loop_desc (loop);
  struct opt_info *opt_info = NULL;
  bool ok;

  if (flag_split_ivs_in_unroller
      || flag_variable_expansion_in_unroller)
    opt_info = analyze_insns_in_loop (loop);


  wont_exit = sbitmap_alloc (nunroll + 1);
  bitmap_clear (wont_exit);
  opt_info_start_duplication (opt_info);

  ok = duplicate_loop_to_header_edge (loop, loop_latch_edge (loop),
				      nunroll, wont_exit,
				      NULL, NULL,
				      DLTHE_FLAG_UPDATE_FREQ
				      | (opt_info
					 ? DLTHE_RECORD_COPY_NUMBER
					   : 0));
  gcc_assert (ok);

  if (opt_info)
    {
      apply_opt_in_copies (opt_info, nunroll, true, true);
      free_opt_info (opt_info);
    }

  free (wont_exit);

  if (desc->simple_p)
    {
      /* We indeed may get here provided that there are nontrivial assumptions
	 for a loop to be really simple.  We could update the counts, but the
	 problem is that we are unable to decide which exit will be taken
	 (not really true in case the number of iterations is constant,
	 but no one will do anything with this information, so we do not
	 worry about it).  */
      desc->simple_p = false;
    }

  if (dump_file)
    fprintf (dump_file, ";; Unrolled loop %d times, %i insns\n",
	     nunroll, num_loop_insns (loop));
}

/* Returns true if REG is referenced in one nondebug insn in LOOP.
   Set *DEBUG_USES to the number of debug insns that reference the
   variable.  */

bool
referenced_in_one_insn_in_loop_p (struct loop *loop, rtx reg,
				  int *debug_uses)
{
  basic_block *body, bb;
  unsigned i;
  int count_ref = 0;
  rtx insn;

  body = get_loop_body (loop);
  for (i = 0; i < loop->num_nodes; i++)
    {
      bb = body[i];

      FOR_BB_INSNS (bb, insn)
	if (!rtx_referenced_p (reg, insn))
	  continue;
	else if (DEBUG_INSN_P (insn))
	  ++*debug_uses;
	else if (++count_ref > 1)
	  break;
    }
  free (body);
  return (count_ref  == 1);
}

/* Reset the DEBUG_USES debug insns in LOOP that reference REG.  */

static void
reset_debug_uses_in_loop (struct loop *loop, rtx reg, int debug_uses)
{
  basic_block *body, bb;
  unsigned i;
  rtx insn;

  body = get_loop_body (loop);
  for (i = 0; debug_uses && i < loop->num_nodes; i++)
    {
      bb = body[i];

      FOR_BB_INSNS (bb, insn)
	if (!DEBUG_INSN_P (insn) || !rtx_referenced_p (reg, insn))
	  continue;
	else
	  {
	    validate_change (insn, &INSN_VAR_LOCATION_LOC (insn),
			     gen_rtx_UNKNOWN_VAR_LOC (), 0);
	    if (!--debug_uses)
	      break;
	  }
    }
  free (body);
}

/* Determine whether INSN contains an accumulator
   which can be expanded into separate copies,
   one for each copy of the LOOP body.

   for (i = 0 ; i < n; i++)
     sum += a[i];

   ==>

   sum += a[i]
   ....
   i = i+1;
   sum1 += a[i]
   ....
   i = i+1
   sum2 += a[i];
   ....

   Return NULL if INSN contains no opportunity for expansion of accumulator.
   Otherwise, allocate a VAR_TO_EXPAND structure, fill it with the relevant
   information and return a pointer to it.
*/

static struct var_to_expand *
analyze_insn_to_expand_var (struct loop *loop, rtx insn)
{
  rtx set, dest, src;
  struct var_to_expand *ves;
  unsigned accum_pos;
  enum rtx_code code;
  int debug_uses = 0;

  set = single_set (insn);
  if (!set)
    return NULL;

  dest = SET_DEST (set);
  src = SET_SRC (set);
  code = GET_CODE (src);

  if (code != PLUS && code != MINUS && code != MULT && code != FMA)
    return NULL;

  if (FLOAT_MODE_P (GET_MODE (dest)))
    {
      if (!flag_associative_math)
        return NULL;
      /* In the case of FMA, we're also changing the rounding.  */
      if (code == FMA && !flag_unsafe_math_optimizations)
	return NULL;
    }

  /* Hmm, this is a bit paradoxical.  We know that INSN is a valid insn
     in MD.  But if there is no optab to generate the insn, we can not
     perform the variable expansion.  This can happen if an MD provides
     an insn but not a named pattern to generate it, for example to avoid
     producing code that needs additional mode switches like for x87/mmx.

     So we check have_insn_for which looks for an optab for the operation
     in SRC.  If it doesn't exist, we can't perform the expansion even
     though INSN is valid.  */
  if (!have_insn_for (code, GET_MODE (src)))
    return NULL;

  if (!REG_P (dest)
      && !(GET_CODE (dest) == SUBREG
           && REG_P (SUBREG_REG (dest))))
    return NULL;

  /* Find the accumulator use within the operation.  */
  if (code == FMA)
    {
      /* We only support accumulation via FMA in the ADD position.  */
      if (!rtx_equal_p  (dest, XEXP (src, 2)))
	return NULL;
      accum_pos = 2;
    }
  else if (rtx_equal_p (dest, XEXP (src, 0)))
    accum_pos = 0;
  else if (rtx_equal_p (dest, XEXP (src, 1)))
    {
      /* The method of expansion that we are using; which includes the
	 initialization of the expansions with zero and the summation of
         the expansions at the end of the computation will yield wrong
	 results for (x = something - x) thus avoid using it in that case.  */
      if (code == MINUS)
	return NULL;
      accum_pos = 1;
    }
  else
    return NULL;

  /* It must not otherwise be used.  */
  if (code == FMA)
    {
      if (rtx_referenced_p (dest, XEXP (src, 0))
	  || rtx_referenced_p (dest, XEXP (src, 1)))
	return NULL;
    }
  else if (rtx_referenced_p (dest, XEXP (src, 1 - accum_pos)))
    return NULL;

  /* It must be used in exactly one insn.  */
  if (!referenced_in_one_insn_in_loop_p (loop, dest, &debug_uses))
    return NULL;

  if (dump_file)
    {
      fprintf (dump_file, "\n;; Expanding Accumulator ");
      print_rtl (dump_file, dest);
      fprintf (dump_file, "\n");
    }

  if (debug_uses)
    /* Instead of resetting the debug insns, we could replace each
       debug use in the loop with the sum or product of all expanded
       accummulators.  Since we'll only know of all expansions at the
       end, we'd have to keep track of which vars_to_expand a debug
       insn in the loop references, take note of each copy of the
       debug insn during unrolling, and when it's all done, compute
       the sum or product of each variable and adjust the original
       debug insn and each copy thereof.  What a pain!  */
    reset_debug_uses_in_loop (loop, dest, debug_uses);

  /* Record the accumulator to expand.  */
  ves = XNEW (struct var_to_expand);
  ves->insn = insn;
  ves->reg = copy_rtx (dest);
  ves->var_expansions.create (1);
  ves->next = NULL;
  ves->op = GET_CODE (src);
  ves->expansion_count = 0;
  ves->reuse_expansion = 0;
  return ves;
}

/* Determine whether there is an induction variable in INSN that
   we would like to split during unrolling.

   I.e. replace

   i = i + 1;
   ...
   i = i + 1;
   ...
   i = i + 1;
   ...

   type chains by

   i0 = i + 1
   ...
   i = i0 + 1
   ...
   i = i0 + 2
   ...

   Return NULL if INSN contains no interesting IVs.  Otherwise, allocate
   an IV_TO_SPLIT structure, fill it with the relevant information and return a
   pointer to it.  */

static struct iv_to_split *
analyze_iv_to_split_insn (rtx insn)
{
  rtx set, dest;
  struct rtx_iv iv;
  struct iv_to_split *ivts;
  bool ok;

  /* For now we just split the basic induction variables.  Later this may be
     extended for example by selecting also addresses of memory references.  */
  set = single_set (insn);
  if (!set)
    return NULL;

  dest = SET_DEST (set);
  if (!REG_P (dest))
    return NULL;

  if (!biv_p (insn, dest))
    return NULL;

  ok = iv_analyze_result (insn, dest, &iv);

  /* This used to be an assert under the assumption that if biv_p returns
     true that iv_analyze_result must also return true.  However, that
     assumption is not strictly correct as evidenced by pr25569.

     Returning NULL when iv_analyze_result returns false is safe and
     avoids the problems in pr25569 until the iv_analyze_* routines
     can be fixed, which is apparently hard and time consuming
     according to their author.  */
  if (! ok)
    return NULL;

  if (iv.step == const0_rtx
      || iv.mode != iv.extend_mode)
    return NULL;

  /* Record the insn to split.  */
  ivts = XNEW (struct iv_to_split);
  ivts->insn = insn;
  ivts->orig_var = dest;
  ivts->base_var = NULL_RTX;
  ivts->step = iv.step;
  ivts->next = NULL;
  ivts->n_loc = 1;
  ivts->loc[0] = 1;

  return ivts;
}

/* Determines which of insns in LOOP can be optimized.
   Return a OPT_INFO struct with the relevant hash tables filled
   with all insns to be optimized.  The FIRST_NEW_BLOCK field
   is undefined for the return value.  */

static struct opt_info *
analyze_insns_in_loop (struct loop *loop)
{
  basic_block *body, bb;
  unsigned i;
  struct opt_info *opt_info = XCNEW (struct opt_info);
  rtx insn;
  struct iv_to_split *ivts = NULL;
  struct var_to_expand *ves = NULL;
  iv_to_split **slot1;
  var_to_expand **slot2;
  vec<edge> edges = get_loop_exit_edges (loop);
  edge exit;
  bool can_apply = false;

  iv_analysis_loop_init (loop);

  body = get_loop_body (loop);

  if (flag_split_ivs_in_unroller)
    {
      opt_info->insns_to_split.create (5 * loop->num_nodes);
      opt_info->iv_to_split_head = NULL;
      opt_info->iv_to_split_tail = &opt_info->iv_to_split_head;
    }

  /* Record the loop exit bb and loop preheader before the unrolling.  */
  opt_info->loop_preheader = loop_preheader_edge (loop)->src;

  if (edges.length () == 1)
    {
      exit = edges[0];
      if (!(exit->flags & EDGE_COMPLEX))
	{
	  opt_info->loop_exit = split_edge (exit);
	  can_apply = true;
	}
    }

  if (flag_variable_expansion_in_unroller
      && can_apply)
    {
      opt_info->insns_with_var_to_expand.create (5 * loop->num_nodes);
      opt_info->var_to_expand_head = NULL;
      opt_info->var_to_expand_tail = &opt_info->var_to_expand_head;
    }

  for (i = 0; i < loop->num_nodes; i++)
    {
      bb = body[i];
      if (!dominated_by_p (CDI_DOMINATORS, loop->latch, bb))
	continue;

      FOR_BB_INSNS (bb, insn)
      {
        if (!INSN_P (insn))
          continue;

        if (opt_info->insns_to_split.is_created ())
          ivts = analyze_iv_to_split_insn (insn);

        if (ivts)
          {
            slot1 = opt_info->insns_to_split.find_slot (ivts, INSERT);
	    gcc_assert (*slot1 == NULL);
            *slot1 = ivts;
	    *opt_info->iv_to_split_tail = ivts;
	    opt_info->iv_to_split_tail = &ivts->next;
            continue;
          }

        if (opt_info->insns_with_var_to_expand.is_created ())
          ves = analyze_insn_to_expand_var (loop, insn);

        if (ves)
          {
            slot2 = opt_info->insns_with_var_to_expand.find_slot (ves, INSERT);
	    gcc_assert (*slot2 == NULL);
            *slot2 = ves;
	    *opt_info->var_to_expand_tail = ves;
	    opt_info->var_to_expand_tail = &ves->next;
          }
      }
    }

  edges.release ();
  free (body);
  return opt_info;
}

/* Called just before loop duplication.  Records start of duplicated area
   to OPT_INFO.  */

static void
opt_info_start_duplication (struct opt_info *opt_info)
{
  if (opt_info)
    opt_info->first_new_block = last_basic_block;
}

/* Determine the number of iterations between initialization of the base
   variable and the current copy (N_COPY).  N_COPIES is the total number
   of newly created copies.  UNROLLING is true if we are unrolling
   (not peeling) the loop.  */

static unsigned
determine_split_iv_delta (unsigned n_copy, unsigned n_copies, bool unrolling)
{
  if (unrolling)
    {
      /* If we are unrolling, initialization is done in the original loop
	 body (number 0).  */
      return n_copy;
    }
  else
    {
      /* If we are peeling, the copy in that the initialization occurs has
	 number 1.  The original loop (number 0) is the last.  */
      if (n_copy)
	return n_copy - 1;
      else
	return n_copies;
    }
}

/* Locate in EXPR the expression corresponding to the location recorded
   in IVTS, and return a pointer to the RTX for this location.  */

static rtx *
get_ivts_expr (rtx expr, struct iv_to_split *ivts)
{
  unsigned i;
  rtx *ret = &expr;

  for (i = 0; i < ivts->n_loc; i++)
    ret = &XEXP (*ret, ivts->loc[i]);

  return ret;
}

/* Allocate basic variable for the induction variable chain.  */

static void
allocate_basic_variable (struct iv_to_split *ivts)
{
  rtx expr = *get_ivts_expr (single_set (ivts->insn), ivts);

  ivts->base_var = gen_reg_rtx (GET_MODE (expr));
}

/* Insert initialization of basic variable of IVTS before INSN, taking
   the initial value from INSN.  */

static void
insert_base_initialization (struct iv_to_split *ivts, rtx insn)
{
  rtx expr = copy_rtx (*get_ivts_expr (single_set (insn), ivts));
  rtx seq;

  start_sequence ();
  expr = force_operand (expr, ivts->base_var);
  if (expr != ivts->base_var)
    emit_move_insn (ivts->base_var, expr);
  seq = get_insns ();
  end_sequence ();

  emit_insn_before (seq, insn);
}

/* Replace the use of induction variable described in IVTS in INSN
   by base variable + DELTA * step.  */

static void
split_iv (struct iv_to_split *ivts, rtx insn, unsigned delta)
{
  rtx expr, *loc, seq, incr, var;
  enum machine_mode mode = GET_MODE (ivts->base_var);
  rtx src, dest, set;

  /* Construct base + DELTA * step.  */
  if (!delta)
    expr = ivts->base_var;
  else
    {
      incr = simplify_gen_binary (MULT, mode,
				  ivts->step, gen_int_mode (delta, mode));
      expr = simplify_gen_binary (PLUS, GET_MODE (ivts->base_var),
				  ivts->base_var, incr);
    }

  /* Figure out where to do the replacement.  */
  loc = get_ivts_expr (single_set (insn), ivts);

  /* If we can make the replacement right away, we're done.  */
  if (validate_change (insn, loc, expr, 0))
    return;

  /* Otherwise, force EXPR into a register and try again.  */
  start_sequence ();
  var = gen_reg_rtx (mode);
  expr = force_operand (expr, var);
  if (expr != var)
    emit_move_insn (var, expr);
  seq = get_insns ();
  end_sequence ();
  emit_insn_before (seq, insn);

  if (validate_change (insn, loc, var, 0))
    return;

  /* The last chance.  Try recreating the assignment in insn
     completely from scratch.  */
  set = single_set (insn);
  gcc_assert (set);

  start_sequence ();
  *loc = var;
  src = copy_rtx (SET_SRC (set));
  dest = copy_rtx (SET_DEST (set));
  src = force_operand (src, dest);
  if (src != dest)
    emit_move_insn (dest, src);
  seq = get_insns ();
  end_sequence ();

  emit_insn_before (seq, insn);
  delete_insn (insn);
}


/* Return one expansion of the accumulator recorded in struct VE.  */

static rtx
get_expansion (struct var_to_expand *ve)
{
  rtx reg;

  if (ve->reuse_expansion == 0)
    reg = ve->reg;
  else
    reg = ve->var_expansions[ve->reuse_expansion - 1];

  if (ve->var_expansions.length () == (unsigned) ve->reuse_expansion)
    ve->reuse_expansion = 0;
  else
    ve->reuse_expansion++;

  return reg;
}


/* Given INSN replace the uses of the accumulator recorded in VE
   with a new register.  */

static void
expand_var_during_unrolling (struct var_to_expand *ve, rtx insn)
{
  rtx new_reg, set;
  bool really_new_expansion = false;

  set = single_set (insn);
  gcc_assert (set);

  /* Generate a new register only if the expansion limit has not been
     reached.  Else reuse an already existing expansion.  */
  if (PARAM_VALUE (PARAM_MAX_VARIABLE_EXPANSIONS) > ve->expansion_count)
    {
      really_new_expansion = true;
      new_reg = gen_reg_rtx (GET_MODE (ve->reg));
    }
  else
    new_reg = get_expansion (ve);

  validate_replace_rtx_group (SET_DEST (set), new_reg, insn);
  if (apply_change_group ())
    if (really_new_expansion)
      {
        ve->var_expansions.safe_push (new_reg);
        ve->expansion_count++;
      }
}

/* Initialize the variable expansions in loop preheader.  PLACE is the
   loop-preheader basic block where the initialization of the
   expansions should take place.  The expansions are initialized with
   (-0) when the operation is plus or minus to honor sign zero.  This
   way we can prevent cases where the sign of the final result is
   effected by the sign of the expansion.  Here is an example to
   demonstrate this:

   for (i = 0 ; i < n; i++)
     sum += something;

   ==>

   sum += something
   ....
   i = i+1;
   sum1 += something
   ....
   i = i+1
   sum2 += something;
   ....

   When SUM is initialized with -zero and SOMETHING is also -zero; the
   final result of sum should be -zero thus the expansions sum1 and sum2
   should be initialized with -zero as well (otherwise we will get +zero
   as the final result).  */

static void
insert_var_expansion_initialization (struct var_to_expand *ve,
				     basic_block place)
{
  rtx seq, var, zero_init;
  unsigned i;
  enum machine_mode mode = GET_MODE (ve->reg);
  bool honor_signed_zero_p = HONOR_SIGNED_ZEROS (mode);

  if (ve->var_expansions.length () == 0)
    return;

  start_sequence ();
  switch (ve->op)
    {
    case FMA:
      /* Note that we only accumulate FMA via the ADD operand.  */
    case PLUS:
    case MINUS:
      FOR_EACH_VEC_ELT (ve->var_expansions, i, var)
        {
	  if (honor_signed_zero_p)
	    zero_init = simplify_gen_unary (NEG, mode, CONST0_RTX (mode), mode);
	  else
	    zero_init = CONST0_RTX (mode);
          emit_move_insn (var, zero_init);
        }
      break;

    case MULT:
      FOR_EACH_VEC_ELT (ve->var_expansions, i, var)
        {
          zero_init = CONST1_RTX (GET_MODE (var));
          emit_move_insn (var, zero_init);
        }
      break;

    default:
      gcc_unreachable ();
    }

  seq = get_insns ();
  end_sequence ();

  emit_insn_after (seq, BB_END (place));
}

/* Combine the variable expansions at the loop exit.  PLACE is the
   loop exit basic block where the summation of the expansions should
   take place.  */

static void
combine_var_copies_in_loop_exit (struct var_to_expand *ve, basic_block place)
{
  rtx sum = ve->reg;
  rtx expr, seq, var, insn;
  unsigned i;

  if (ve->var_expansions.length () == 0)
    return;

  start_sequence ();
  switch (ve->op)
    {
    case FMA:
      /* Note that we only accumulate FMA via the ADD operand.  */
    case PLUS:
    case MINUS:
      FOR_EACH_VEC_ELT (ve->var_expansions, i, var)
	sum = simplify_gen_binary (PLUS, GET_MODE (ve->reg), var, sum);
      break;

    case MULT:
      FOR_EACH_VEC_ELT (ve->var_expansions, i, var)
	sum = simplify_gen_binary (MULT, GET_MODE (ve->reg), var, sum);
      break;

    default:
      gcc_unreachable ();
    }

  expr = force_operand (sum, ve->reg);
  if (expr != ve->reg)
    emit_move_insn (ve->reg, expr);
  seq = get_insns ();
  end_sequence ();

  insn = BB_HEAD (place);
  while (!NOTE_INSN_BASIC_BLOCK_P (insn))
    insn = NEXT_INSN (insn);

  emit_insn_after (seq, insn);
}

/* Strip away REG_EQUAL notes for IVs we're splitting.

   Updating REG_EQUAL notes for IVs we split is tricky: We
   cannot tell until after unrolling, DF-rescanning, and liveness
   updating, whether an EQ_USE is reached by the split IV while
   the IV reg is still live.  See PR55006.

   ??? We cannot use remove_reg_equal_equiv_notes_for_regno,
   because RTL loop-iv requires us to defer rescanning insns and
   any notes attached to them.  So resort to old techniques...  */

static void
maybe_strip_eq_note_for_split_iv (struct opt_info *opt_info, rtx insn)
{
  struct iv_to_split *ivts;
  rtx note = find_reg_equal_equiv_note (insn);
  if (! note)
    return;
  for (ivts = opt_info->iv_to_split_head; ivts; ivts = ivts->next)
    if (reg_mentioned_p (ivts->orig_var, note))
      {
	remove_note (insn, note);
	return;
      }
}

/* Apply loop optimizations in loop copies using the
   data which gathered during the unrolling.  Structure
   OPT_INFO record that data.

   UNROLLING is true if we unrolled (not peeled) the loop.
   REWRITE_ORIGINAL_BODY is true if we should also rewrite the original body of
   the loop (as it should happen in complete unrolling, but not in ordinary
   peeling of the loop).  */

static void
apply_opt_in_copies (struct opt_info *opt_info,
                     unsigned n_copies, bool unrolling,
                     bool rewrite_original_loop)
{
  unsigned i, delta;
  basic_block bb, orig_bb;
  rtx insn, orig_insn, next;
  struct iv_to_split ivts_templ, *ivts;
  struct var_to_expand ve_templ, *ves;

  /* Sanity check -- we need to put initialization in the original loop
     body.  */
  gcc_assert (!unrolling || rewrite_original_loop);

  /* Allocate the basic variables (i0).  */
  if (opt_info->insns_to_split.is_created ())
    for (ivts = opt_info->iv_to_split_head; ivts; ivts = ivts->next)
      allocate_basic_variable (ivts);

  for (i = opt_info->first_new_block; i < (unsigned) last_basic_block; i++)
    {
      bb = BASIC_BLOCK_FOR_FN (cfun, i);
      orig_bb = get_bb_original (bb);

      /* bb->aux holds position in copy sequence initialized by
	 duplicate_loop_to_header_edge.  */
      delta = determine_split_iv_delta ((size_t)bb->aux, n_copies,
					unrolling);
      bb->aux = 0;
      orig_insn = BB_HEAD (orig_bb);
      FOR_BB_INSNS_SAFE (bb, insn, next)
        {
	  if (!INSN_P (insn)
	      || (DEBUG_INSN_P (insn)
		  && TREE_CODE (INSN_VAR_LOCATION_DECL (insn)) == LABEL_DECL))
            continue;

	  while (!INSN_P (orig_insn)
		 || (DEBUG_INSN_P (orig_insn)
		     && (TREE_CODE (INSN_VAR_LOCATION_DECL (orig_insn))
			 == LABEL_DECL)))
            orig_insn = NEXT_INSN (orig_insn);

          ivts_templ.insn = orig_insn;
          ve_templ.insn = orig_insn;

          /* Apply splitting iv optimization.  */
          if (opt_info->insns_to_split.is_created ())
            {
	      maybe_strip_eq_note_for_split_iv (opt_info, insn);

              ivts = opt_info->insns_to_split.find (&ivts_templ);

              if (ivts)
                {
		  gcc_assert (GET_CODE (PATTERN (insn))
			      == GET_CODE (PATTERN (orig_insn)));

                  if (!delta)
                    insert_base_initialization (ivts, insn);
                  split_iv (ivts, insn, delta);
                }
            }
          /* Apply variable expansion optimization.  */
          if (unrolling && opt_info->insns_with_var_to_expand.is_created ())
            {
              ves = (struct var_to_expand *)
		opt_info->insns_with_var_to_expand.find (&ve_templ);
              if (ves)
                {
		  gcc_assert (GET_CODE (PATTERN (insn))
			      == GET_CODE (PATTERN (orig_insn)));
                  expand_var_during_unrolling (ves, insn);
                }
            }
          orig_insn = NEXT_INSN (orig_insn);
        }
    }

  if (!rewrite_original_loop)
    return;

  /* Initialize the variable expansions in the loop preheader
     and take care of combining them at the loop exit.  */
  if (opt_info->insns_with_var_to_expand.is_created ())
    {
      for (ves = opt_info->var_to_expand_head; ves; ves = ves->next)
	insert_var_expansion_initialization (ves, opt_info->loop_preheader);
      for (ves = opt_info->var_to_expand_head; ves; ves = ves->next)
	combine_var_copies_in_loop_exit (ves, opt_info->loop_exit);
    }

  /* Rewrite also the original loop body.  Find them as originals of the blocks
     in the last copied iteration, i.e. those that have
     get_bb_copy (get_bb_original (bb)) == bb.  */
  for (i = opt_info->first_new_block; i < (unsigned) last_basic_block; i++)
    {
      bb = BASIC_BLOCK_FOR_FN (cfun, i);
      orig_bb = get_bb_original (bb);
      if (get_bb_copy (orig_bb) != bb)
	continue;

      delta = determine_split_iv_delta (0, n_copies, unrolling);
      for (orig_insn = BB_HEAD (orig_bb);
           orig_insn != NEXT_INSN (BB_END (bb));
           orig_insn = next)
        {
          next = NEXT_INSN (orig_insn);

          if (!INSN_P (orig_insn))
 	    continue;

          ivts_templ.insn = orig_insn;
          if (opt_info->insns_to_split.is_created ())
            {
	      maybe_strip_eq_note_for_split_iv (opt_info, orig_insn);

              ivts = (struct iv_to_split *)
		opt_info->insns_to_split.find (&ivts_templ);
              if (ivts)
                {
                  if (!delta)
                    insert_base_initialization (ivts, orig_insn);
                  split_iv (ivts, orig_insn, delta);
                  continue;
                }
            }

        }
    }
}

/* Release OPT_INFO.  */

static void
free_opt_info (struct opt_info *opt_info)
{
  if (opt_info->insns_to_split.is_created ())
    opt_info->insns_to_split.dispose ();
  if (opt_info->insns_with_var_to_expand.is_created ())
    {
      struct var_to_expand *ves;

      for (ves = opt_info->var_to_expand_head; ves; ves = ves->next)
	ves->var_expansions.release ();
      opt_info->insns_with_var_to_expand.dispose ();
    }
  free (opt_info);
}
