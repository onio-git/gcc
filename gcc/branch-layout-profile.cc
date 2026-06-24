/* Data-driven conditional-branch layout (GIMPLE injection).
   Overrides GIMPLE_COND edge probabilities from an external profile so that the
   downstream frequency propagation and bb-reorder rotate the measured-hot
   successor to fall-through.  Called from tree_estimate_probability after the
   static predictors combine and before estimate_bb_frequencies, so the new
   probabilities propagate into counts naturally.

   Profile lines: "basename:line:disc>targetline taken total".  The target line
   disambiguates tail-duplicated copies that share branch line:disc but jump to
   different continuations.  Enabled when env BRANCH_PROFILE names a file. */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "tree.h"
#include "gimple.h"
#include "cfghooks.h"
#include "gimple-iterator.h"
#include "tree-cfg.h"
#include <map>
#include <string>

static std::map<std::string, std::pair<long, long> > *blp_map;
static bool blp_tried;

static void
blp_load (void)
{
  if (blp_tried)
    return;
  blp_tried = true;
  const char *path = getenv ("BRANCH_PROFILE");
  if (!path)
    return;
  FILE *f = fopen (path, "r");
  if (!f)
    return;
  blp_map = new std::map<std::string, std::pair<long, long> > ();
  char key[600];
  long t, e;
  while (fscanf (f, "%599s %ld %ld", key, &t, &e) == 3)
    (*blp_map)[std::string (key)] = std::make_pair (t, e);
  fclose (f);
}

/* Representative source line of BB: first located real stmt, following empty
   single-successor forwarders a couple of hops. */
static int
blp_bb_line (basic_block bb)
{
  for (int hop = 0; bb && hop < 4; hop++)
    {
      gimple_stmt_iterator gsi;
      bool empty = true;
      for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
	{
	  gimple *s = gsi_stmt (gsi);
	  if (is_gimple_debug (s) || gimple_code (s) == GIMPLE_LABEL)
	    continue;
	  empty = false;
	  location_t loc = gimple_location (s);
	  if (loc != UNKNOWN_LOCATION)
	    return expand_location (loc).line;
	  break;
	}
      if (!empty || !single_succ_p (bb))
	break;
      bb = single_succ (bb);
    }
  return 0;
}

void
apply_branch_layout_profile (void)
{
  /* Forced-flip probe: swap the two successor edge probabilities of every
     conditional in the named function(s), so bb-reorder rotates the OTHER
     successor to fall-through.  FSM_FLIP names a function (substring match) or
     "ALL".  This is a deliberate anti-layout test for the hot branches the
     data-driven profile cannot key -- e.g. tail-duplicated FSM copies whose
     split taken-rates straddle the filter and get rejected.  */
  const char *flip = getenv ("FSM_FLIP");
  if (flip && cfun && cfun->decl && DECL_NAME (cfun->decl))
    {
      const char *fn = IDENTIFIER_POINTER (DECL_NAME (cfun->decl));
      if (!strcmp (flip, "ALL") || strstr (flip, fn))
	{
	  basic_block bb;
	  int n = 0;
	  FOR_EACH_BB_FN (bb, cfun)
	    {
	      gimple_stmt_iterator gsi = gsi_last_bb (bb);
	      while (!gsi_end_p (gsi) && is_gimple_debug (gsi_stmt (gsi)))
		gsi_prev (&gsi);
	      gimple *stmt = gsi_end_p (gsi) ? NULL : gsi_stmt (gsi);
	      if (!stmt || gimple_code (stmt) != GIMPLE_COND)
		continue;
	      if (EDGE_COUNT (bb->succs) != 2)
		continue;
	      edge e0 = EDGE_SUCC (bb, 0), e1 = EDGE_SUCC (bb, 1);
	      profile_probability p = e0->probability;
	      e0->probability = e1->probability;
	      e1->probability = p;
	      n++;
	    }
	  if (getenv ("FSM_FLIP_DEBUG"))
	    fprintf (stderr, "FSM_FLIP: %s swapped %d conds\n", fn, n);
	  return;
	}
    }
  blp_load ();
  if (!blp_map)
    return;
  basic_block bb;
  FOR_EACH_BB_FN (bb, cfun)
    {
      gimple_stmt_iterator gsi = gsi_last_bb (bb);
      while (!gsi_end_p (gsi) && is_gimple_debug (gsi_stmt (gsi)))
	gsi_prev (&gsi);
      gimple *stmt = gsi_end_p (gsi) ? NULL : gsi_stmt (gsi);
      if (!stmt || gimple_code (stmt) != GIMPLE_COND)
	continue;
      if (EDGE_COUNT (bb->succs) != 2)
	continue;
      location_t loc = gimple_location (stmt);
      if (loc == UNKNOWN_LOCATION)
	continue;
      expanded_location xl = expand_location (loc);
      if (!xl.file)
	continue;
      int disc = get_discriminator_from_loc (loc);
      char base[450];
      snprintf (base, sizeof base, "%s:%d:%d", lbasename (xl.file), xl.line, disc);

      edge e0 = EDGE_SUCC (bb, 0), e1 = EDGE_SUCC (bb, 1);
      char k0[520], k1[520];
      snprintf (k0, sizeof k0, "%s>%d", base, blp_bb_line (e0->dest));
      snprintf (k1, sizeof k1, "%s>%d", base, blp_bb_line (e1->dest));
      std::map<std::string, std::pair<long, long> >::iterator
	it0 = blp_map->find (std::string (k0)),
	it1 = blp_map->find (std::string (k1)),
	end = blp_map->end ();

      edge taken = NULL;
      long t = 0, total = 0;
      if (it0 != end && it1 == end)
	{ taken = e0; t = it0->second.first; total = it0->second.second; }
      else if (it1 != end && it0 == end)
	{ taken = e1; t = it1->second.first; total = it1->second.second; }
      else
	continue;
      if (total <= 0)
	continue;
      int v = (int) ((double) t * REG_BR_PROB_BASE / total + 0.5);
      if (v < 1)
	v = 1;
      if (v > REG_BR_PROB_BASE - 1)
	v = REG_BR_PROB_BASE - 1;
      edge other = (taken == e0) ? e1 : e0;
      taken->probability = profile_probability::from_reg_br_prob_base (v);
      other->probability = taken->probability.invert ();
    }
}
