/* Post-reload loop spill motion: hoist loop-invariant spill-slot reloads and
   sink loop-exit-only spill-slot stores.

   LRA sometimes leaves spill-slot traffic inside an inner loop that the
   pre-reload loop optimizers never saw: a loop-invariant value reloaded from
   its stack slot every iteration, or a value stored to its slot every
   iteration when only the post-loop value is read (e.g. a search key and a
   found-element field inside a list traversal nested in a high-pressure
   mergesort, where register pressure forced both into the frame).

   This pass runs after reload.  For each innermost loop it:

     * hoists an invariant reload  dst = mem[slot]  -- slot never written in the
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
#include "rtl-iter.h"
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

/* A byte range within GCC's artificial spill-slot object.  Allocator-created
   spill MEMs are tagged with get_spill_slot_decl; a stack-relative address by
   itself is not enough, since it can also denote an ordinary user object.  */

struct spill_slot
{
  HOST_WIDE_INT offset;
  HOST_WIDE_INT size;
};

/* True if X is a nonvolatile allocator-created spill-slot reference, and
   store its byte range in *SLOT.  */

static bool
spill_slot_ref_p (const_rtx x, spill_slot *slot)
{
  tree spill_decl = get_spill_slot_decl (false);
  if (!MEM_P (x)
      || MEM_VOLATILE_P (x)
      || !MEM_NOTRAP_P (x)
      || !spill_decl
      || MEM_EXPR (x) != spill_decl
      || !MEM_OFFSET_KNOWN_P (x)
      || !MEM_SIZE_KNOWN_P (x))
    return false;

  HOST_WIDE_INT offset, size;
  if (!MEM_OFFSET (x).is_constant (&offset)
      || !MEM_SIZE (x).is_constant (&size)
      || size <= 0)
    return false;

  slot->offset = offset;
  slot->size = size;
  return true;
}

/* True if A and B have at least one byte in common.  */

static bool
spill_slots_overlap_p (const spill_slot &a, const spill_slot &b)
{
  return ranges_maybe_overlap_p (a.offset, a.size, b.offset, b.size);
}

/* True if OUTER completely covers INNER.  */

static bool
spill_slot_covers_p (const spill_slot &outer, const spill_slot &inner)
{
  return known_subrange_p (inner.offset, inner.size,
			   outer.offset, outer.size);
}

/* Helper for note_stores: collect spill-slot ranges written in the loop and
   note any store to an unrecognized memory location.  */

struct store_info
{
  auto_vec<spill_slot> *slots;
  bool *unknown;
};

static void
record_store (rtx dest, const_rtx, void *data)
{
  store_info *si = (store_info *) data;
  if (!MEM_P (dest))
    return;
  spill_slot slot;
  if (spill_slot_ref_p (dest, &slot))
    si->slots->safe_push (slot);
  else
    *si->unknown = true;
}

/* Helper for note_uses: collect every spill-slot range read by an insn.  */

static void
record_load (rtx *x, void *data)
{
  auto_vec<spill_slot> *slots = (auto_vec<spill_slot> *) data;
  subrtx_iterator::array_type array;
  FOR_EACH_SUBRTX (iter, array, *x, NONCONST)
    if (MEM_P (*iter))
      {
	spill_slot slot;
	if (spill_slot_ref_p (*iter, &slot))
	  slots->safe_push (slot);
      }
}

/* True if BB completely overwrites SLOT before any read or partial write of
   it -- i.e. the range is dead on entry to BB.  Conservatively false if BB
   never proves the overwrite.  */

static bool
slot_dead_at (basic_block bb, const spill_slot &slot)
{
  rtx_insn *insn;
  FOR_BB_INSNS (bb, insn)
    {
      if (!NONDEBUG_INSN_P (insn))
	continue;

      auto_vec<spill_slot> reads;
      note_uses (&PATTERN (insn), record_load, &reads);
      for (const spill_slot &read : reads)
	if (spill_slots_overlap_p (read, slot))
	  return false;

      auto_vec<spill_slot> writes;
      bool unknown_store = false;
      store_info si = { &writes, &unknown_store };
      note_stores (insn, record_store, &si);
      for (const spill_slot &write : writes)
	if (spill_slots_overlap_p (write, slot))
	  return spill_slot_covers_p (write, slot);
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
  auto_vec<spill_slot> stored_slots;
  auto_vec<spill_slot> loaded_slots;
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
	  note_uses (&PATTERN (insn), record_load, &loaded_slots);
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

  /* (1) Hoist invariant reloads "dst = mem[slot]" into the preheader, keeping
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
	  if (RTX_FRAME_RELATED_P (insn))
	    continue;
	  rtx set = PATTERN (insn);
	  if (GET_CODE (set) != SET)
	    continue;
	  rtx dst = SET_DEST (set);
	  rtx src = SET_SRC (set);
	  spill_slot slot;
	  if (!REG_P (dst) || REGNO (dst) >= FIRST_PSEUDO_REGISTER)
	    continue;
	  if (!spill_slot_ref_p (src, &slot))
	    continue;
	  if (unknown_store)
	    continue;
	  bool stored = false;
	  for (const spill_slot &stored_slot : stored_slots)
	    if (spill_slots_overlap_p (stored_slot, slot))
	      {
		stored = true;
		break;
	      }
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
	  INSN_LOCATION (load) = INSN_LOCATION (insn);
	  if (last && JUMP_P (last))
	    emit_insn_before (load, last);
	  else
	    emit_insn_after (load, last);
	  df_insn_rescan (load);
	  delete_insn (insn);

	  if (dump_file)
	    fprintf (dump_file,
		     "  hoisted invariant reload of slot %ld+%ld (%s) "
		     "out of loop %d\n",
		     (long) slot.offset, (long) slot.size,
		     reg_names[dn], loop->num);
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
	  if (RTX_FRAME_RELATED_P (insn))
	    continue;
	  rtx set = PATTERN (insn);
	  if (GET_CODE (set) != SET)
	    continue;
	  rtx dst = SET_DEST (set);
	  rtx src = SET_SRC (set);
	  spill_slot slot;
	  if (!spill_slot_ref_p (dst, &slot))
	    continue;
	  if (!REG_P (src) || REGNO (src) >= FIRST_PSEUDO_REGISTER)
	    continue;
	  if (unknown_store)
	    continue;

	  bool readed = false;
	  for (const spill_slot &loaded_slot : loaded_slots)
	    if (spill_slots_overlap_p (loaded_slot, slot))
	      {
		readed = true;
		break;
	      }
	  if (readed)
	    continue;
	  int wc = 0;
	  for (const spill_slot &stored_slot : stored_slots)
	    if (spill_slots_overlap_p (stored_slot, slot))
	      wc++;
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
	      if (x->flags & (EDGE_ABNORMAL | EDGE_EH | EDGE_FAKE))
		{
		  ok = false;
		  break;
		}
	      if (x->src == b)
		has_b_exit = true;
	      else if (!slot_dead_at (x->dest, slot))
		{ ok = false; break; }
	    }
	  if (!ok || !has_b_exit)
	    continue;

	  for (edge x : exits)
	    if (x->src == b)
	      {
		rtx_insn *store = gen_move_insn (copy_rtx (dst), copy_rtx (src));
		INSN_LOCATION (store) = INSN_LOCATION (insn);
		insert_insn_on_edge (store, x);
	      }
	  delete_insn (insn);

	  if (dump_file)
	    fprintf (dump_file,
		     "  sank exit-only store of slot %ld+%ld (%s) "
		     "out of loop %d\n",
		     (long) slot.offset, (long) slot.size,
		     reg_names[REGNO (src)], loop->num);
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
  TODO_df_finish, /* todo_flags_finish */
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

    /* Creating preheaders and simple latches can split blocks.  Recompute
       dataflow afterwards: process_loop uses live-in information for the
       resulting loop headers, and a newly-created header otherwise has stale
       (usually empty) liveness.  Treating a live hard register as dead can
       make a conditional spill reload look safe to hoist and overwrite the
       value carried around an enclosing loop.  */
    df_analyze ();

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
