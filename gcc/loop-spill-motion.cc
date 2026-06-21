/* Post-reload loop spill motion: hoist loop-invariant spill-slot reloads and
   sink loop-exit-only spill-slot stores.

   LRA sometimes leaves spill-slot traffic inside an inner loop that the
   pre-reload loop optimizers never saw: a loop-invariant value reloaded from
   its stack slot every iteration, or a value stored to its slot every
   iteration when only the post-loop value is read (e.g. a search key and a
   found-element field inside a list traversal nested in a high-pressure
   mergesort, where register pressure forced both into the frame).

   This pass runs after reload.  For each innermost loop it:

     * sinks an invariant reload  dst = mem[slot]  -- slot never written in the
       loop, dst a single hard register defined once and dead on entry -- into
       the preheader, keeping the same destination register and deleting the
       per-iteration load; and

     * sinks a store  mem[slot] = src  -- slot never read and written exactly
       once in the loop, src still holding the stored value at the loop exit,
       and every other loop exit overwriting the slot -- onto the loop-exit
       edges, deleting the per-iteration store.

   Both eliminate the per-iteration memory access outright.  No existing pass
   (gcse2/postreload-gcse, ree, regcprop, ...) recovers these, because the
   accesses are spill slots created by LRA, after the loop optimizers run.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "target.h"
#include "rtl.h"
#include "tree.h"
#include "predict.h"
#include "df.h"
#include "memmodel.h"
#include "tm_p.h"
#include "insn-config.h"
#include "regs.h"
#include "recog.h"
#include "cfgrtl.h"
#include "cfghooks.h"
#include "cfgcleanup.h"
#include "cfgloop.h"
#include "emit-rtl.h"
#include "expr.h"
#include "tree-pass.h"
#include "print-rtl.h"

/* True if X is a reference to a frame-relative spill slot (sp/fp + const),
   and store its byte offset in *OFF.  */

static bool
spill_slot_ref_p (rtx x, HOST_WIDE_INT *off)
{
  if (!MEM_P (x))
    return false;
  rtx a = XEXP (x, 0);
  if (GET_CODE (a) == PLUS && REG_P (XEXP (a, 0)) && CONST_INT_P (XEXP (a, 1)))
    {
      unsigned r = REGNO (XEXP (a, 0));
      if (r == STACK_POINTER_REGNUM || r == HARD_FRAME_POINTER_REGNUM
	  || r == FRAME_POINTER_REGNUM || r == ARG_POINTER_REGNUM)
	{
	  *off = INTVAL (XEXP (a, 1));
	  return true;
	}
    }
  return false;
}

/* Helper for note_stores: collect frame-slot offsets written in the loop and
   note any store to a non-frame (unknown) memory location.  */

struct store_info
{
  auto_vec<HOST_WIDE_INT> *slots;
  bool *unknown;
};

static void
record_store (rtx dest, const_rtx, void *data)
{
  store_info *si = (store_info *) data;
  if (!MEM_P (dest))
    return;
  HOST_WIDE_INT off;
  if (spill_slot_ref_p (dest, &off))
    si->slots->safe_push (off);
  else
    *si->unknown = true;
}

/* True if BB writes spill slot OFF before any read of it -- i.e. the slot is
   dead on entry to BB.  Conservatively false if BB never touches the slot.  */

static bool
slot_dead_at (basic_block bb, HOST_WIDE_INT off)
{
  rtx_insn *insn;
  FOR_BB_INSNS (bb, insn)
    {
      if (!NONDEBUG_INSN_P (insn))
	continue;
      rtx set = single_set (insn);
      if (!set)
	continue;
      HOST_WIDE_INT o;
      if (spill_slot_ref_p (SET_SRC (set), &o) && o == off)
	return false;
      if (spill_slot_ref_p (SET_DEST (set), &o) && o == off)
	return true;
    }
  return false;
}

/* Process one innermost loop; return the number of transforms applied.  */

static int
process_loop (class loop *loop)
{
  edge pre = loop_preheader_edge (loop);
  if (!pre || pre->src == ENTRY_BLOCK_PTR_FOR_FN (cfun))
    return 0;

  basic_block ph = NULL;			/* lazy preheader insert block */
  basic_block *bbs = get_loop_body (loop);
  auto_vec<HOST_WIDE_INT> stored_slots;
  auto_vec<HOST_WIDE_INT> loaded_slots;
  bool unknown_store = false;

  /* First scan: stored/loaded spill slots, stores to unknown memory, and how
     many times each hard register is defined in the loop.  */
  unsigned char defcount[FIRST_PSEUDO_REGISTER];
  memset (defcount, 0, sizeof (defcount));
  for (unsigned i = 0; i < loop->num_nodes; i++)
    {
      rtx_insn *insn;
      FOR_BB_INSNS (bbs[i], insn)
	{
	  if (!NONDEBUG_INSN_P (insn))
	    continue;
	  store_info si = { &stored_slots, &unknown_store };
	  note_stores (insn, record_store, &si);
	  rtx s = single_set (insn);
	  HOST_WIDE_INT lo;
	  if (s && spill_slot_ref_p (SET_SRC (s), &lo))
	    loaded_slots.safe_push (lo);
	  df_ref def;
	  FOR_EACH_INSN_DEF (def, insn)
	    {
	      unsigned r = DF_REF_REGNO (def);
	      if (r < FIRST_PSEUDO_REGISTER && defcount[r] < 255)
		defcount[r]++;
	    }
	}
    }

  bitmap lin = df_get_live_in (loop->header);
  int changed = 0;

  /* (1) Sink invariant reloads "dst = mem[slot]" into the preheader, keeping
     DST.  Safe when the slot is never written in the loop and DST is a single
     hard register defined exactly once (by this insn) and dead on entry, so
     every in-loop read of DST observes this load's (invariant) value.  */
  for (unsigned i = 0; i < loop->num_nodes; i++)
    {
      rtx_insn *insn, *nexti;
      FOR_BB_INSNS_SAFE (bbs[i], insn, nexti)
	{
	  if (!NONDEBUG_INSN_P (insn))
	    continue;
	  rtx set = single_set (insn);
	  if (!set)
	    continue;
	  rtx dst = SET_DEST (set);
	  rtx src = SET_SRC (set);
	  HOST_WIDE_INT off;
	  if (!REG_P (dst) || REGNO (dst) >= FIRST_PSEUDO_REGISTER)
	    continue;
	  if (!spill_slot_ref_p (src, &off))
	    continue;
	  if (unknown_store)
	    continue;
	  bool stored = false;
	  for (HOST_WIDE_INT s : stored_slots)
	    if (s == off) { stored = true; break; }
	  if (stored)
	    continue;

	  unsigned dn = REGNO (dst);
	  machine_mode mode = GET_MODE (dst);
	  if (hard_regno_nregs (dn, mode) != 1)
	    continue;
	  if (defcount[dn] != 1)
	    continue;
	  if (REGNO_REG_SET_P (lin, dn))
	    continue;

	  /* Land the load at the end of the single-successor preheader: before
	     a terminating jump, else after the last insn.  No CFG change.  */
	  if (!ph)
	    ph = pre->src;
	  rtx_insn *last = BB_END (ph);
	  rtx_insn *load = gen_move_insn (copy_rtx (dst), copy_rtx (src));
	  if (last && JUMP_P (last))
	    emit_insn_before (load, last);
	  else
	    emit_insn_after (load, last);
	  df_insn_rescan (load);
	  delete_insn (insn);

	  if (dump_file)
	    fprintf (dump_file,
		     "  sank invariant reload of slot %ld (%s) out of loop %d\n",
		     (long) off, reg_names[dn], loop->num);
	  changed++;
	}
    }

  /* (2) Sink stores "mem[slot] = src" onto the loop-exit edges.  Safe when the
     slot is never read in the loop and written exactly once, SRC still holds
     the stored value at the storing block's exit, and every loop exit either
     leaves from that block (where SRC is current) or lands in a block that
     overwrites the slot (so the missing per-iteration store cannot be seen).  */
  auto_vec<edge> exits = get_loop_exit_edges (loop);
  for (unsigned i = 0; i < loop->num_nodes; i++)
    {
      rtx_insn *insn, *nexti;
      FOR_BB_INSNS_SAFE (bbs[i], insn, nexti)
	{
	  if (!NONDEBUG_INSN_P (insn))
	    continue;
	  rtx set = single_set (insn);
	  if (!set)
	    continue;
	  rtx dst = SET_DEST (set);
	  rtx src = SET_SRC (set);
	  HOST_WIDE_INT off;
	  if (!spill_slot_ref_p (dst, &off))
	    continue;
	  if (!REG_P (src) || REGNO (src) >= FIRST_PSEUDO_REGISTER)
	    continue;
	  if (unknown_store)
	    continue;

	  bool readed = false;
	  for (HOST_WIDE_INT lo : loaded_slots)
	    if (lo == off) { readed = true; break; }
	  if (readed)
	    continue;
	  int wc = 0;
	  for (HOST_WIDE_INT so : stored_slots)
	    if (so == off) wc++;
	  if (wc != 1)
	    continue;

	  basic_block b = BLOCK_FOR_INSN (insn);
	  /* SRC must be unmodified from the store to B's exit.  */
	  bool clob = false;
	  for (rtx_insn *j = NEXT_INSN (insn);
	       j && j != NEXT_INSN (BB_END (b)); j = NEXT_INSN (j))
	    if (NONDEBUG_INSN_P (j) && reg_set_p (src, j))
	      { clob = true; break; }
	  if (clob)
	    continue;

	  bool ok = true, has_b_exit = false;
	  for (edge x : exits)
	    {
	      if (x->src == b)
		{
		  if (!(x->flags & EDGE_ABNORMAL))
		    has_b_exit = true;
		}
	      else if (!slot_dead_at (x->dest, off))
		{ ok = false; break; }
	    }
	  if (!ok || !has_b_exit)
	    continue;

	  for (edge x : exits)
	    if (x->src == b && !(x->flags & EDGE_ABNORMAL))
	      insert_insn_on_edge (gen_move_insn (copy_rtx (dst),
						  copy_rtx (src)), x);
	  delete_insn (insn);

	  if (dump_file)
	    fprintf (dump_file,
		     "  sank exit-only store of slot %ld (%s) out of loop %d\n",
		     (long) off, reg_names[REGNO (src)], loop->num);
	  changed++;
	}
    }

  free (bbs);
  return changed;
}

namespace {

const pass_data pass_data_loop_spill_motion =
{
  RTL_PASS, /* type */
  "spillmotion", /* name */
  OPTGROUP_LOOP, /* optinfo_flags */
  TV_NONE, /* tv_id */
  0, /* properties_required */
  0, /* properties_provided */
  0, /* properties_destroyed */
  0, /* todo_flags_start */
  0, /* todo_flags_finish */
};

class pass_loop_spill_motion : public rtl_opt_pass
{
public:
  pass_loop_spill_motion (gcc::context *ctxt)
    : rtl_opt_pass (pass_data_loop_spill_motion, ctxt)
  {}

  bool gate (function *fun) final override
  {
    return optimize > 0 && flag_loop_spill_motion
	   && optimize_function_for_speed_p (fun);
  }

  unsigned int execute (function *) final override
  {
    df_set_flags (DF_LR_RUN_DCE);
    df_note_add_problem ();
    df_analyze ();

    loop_optimizer_init (LOOPS_NORMAL | LOOPS_HAVE_PREHEADERS
			 | LOOPS_HAVE_SIMPLE_LATCHES);

    int total = 0;
    for (auto loop : loops_list (cfun, LI_ONLY_INNERMOST))
      total += process_loop (loop);

    /* Tear down loops as pass_rtl_loop_done does, with the deferred exit-edge
       stores committed in between: finalize, commit, drop the now-stale
       dominator tree, then the RTL cfg cleanup (cleanup_cfg, NOT the GIMPLE
       TODO_cleanup_cfg).  */
    loop_optimizer_finalize ();
    commit_edge_insertions ();
    free_dominance_info (CDI_DOMINATORS);
    cleanup_cfg (0);

    if (total && dump_file)
      fprintf (dump_file, "loop-spill-motion: %d transform(s)\n", total);
    return 0;
  }

}; // class pass_loop_spill_motion

} // anon namespace

rtl_opt_pass *
make_pass_loop_spill_motion (gcc::context *ctxt)
{
  return new pass_loop_spill_motion (ctxt);
}
