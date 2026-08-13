/* Data-driven conditional-branch layout (GIMPLE injection).
   Overrides GIMPLE_COND edge probabilities from an external profile so that the
   downstream frequency propagation and bb-reorder rotate the measured-hot
   successor to fall-through.  Called from tree_estimate_probability after the
   static predictors combine and before estimate_bb_frequencies, so the new
   probabilities propagate into counts naturally.

   Profile lines: "basename:line:disc>targetline taken total".  The target line
   disambiguates tail-duplicated copies that share branch line:disc but jump to
   different continuations.  Enabled by -fbranch-layout-profile=FILE.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "tree.h"
#include "gimple.h"
#include "diagnostic-core.h"
#include "cfghooks.h"
#include "gimple-iterator.h"
#include "tree-cfg.h"
#include <map>
#include <string>

typedef std::pair<unsigned long long, unsigned long long> blp_count;
static std::map<std::string, blp_count> *blp_map;
static bool blp_tried;

/* Parse an unsigned decimal counter without accepting signs or wrapping on
   overflow (strtoull accepts a leading minus sign).  */

static bool
blp_parse_count (const char *text, unsigned long long *result)
{
  if (!*text)
    return false;

  unsigned long long value = 0;
  for (const unsigned char *p = (const unsigned char *) text; *p; ++p)
    {
      if (!ISDIGIT (*p))
	return false;
      unsigned int digit = *p - '0';
      if (value > (~0ULL - digit) / 10)
	return false;
      value = value * 10 + digit;
    }
  *result = value;
  return true;
}

static void
blp_load (void)
{
  if (blp_tried)
    return;
  blp_tried = true;
  const char *path = branch_layout_profile_file;
  if (!path)
    return;
  FILE *f = fopen (path, "r");
  if (!f)
    {
      error ("cannot open branch layout profile %qs: %m", path);
      return;
    }

  blp_map = new std::map<std::string, blp_count> ();
  char line[1024];
  unsigned int lineno = 0;
  while (fgets (line, sizeof line, f))
    {
      lineno++;
      if (!strchr (line, '\n') && !feof (f))
	{
	  error ("branch layout profile %qs:%u has an overlong line",
		 path, lineno);
	  break;
	}

      char *comment = strchr (line, '#');
      if (comment)
	*comment = '\0';
      char *p = line;
      while (ISSPACE (*p))
	p++;
      if (!*p)
	continue;

      char key[600], taken_text[64], total_text[64], extra;
      unsigned long long taken, total;
      if (sscanf (p, "%599s %63s %63s %c", key, taken_text, total_text,
		  &extra) != 3
	  || !blp_parse_count (taken_text, &taken)
	  || !blp_parse_count (total_text, &total))
	{
	  error ("malformed branch layout profile entry at %qs:%u",
		 path, lineno);
	  continue;
	}
      if (!total || taken > total)
	{
	  error ("invalid branch counts at %qs:%u", path, lineno);
	  continue;
	}
      (*blp_map)[std::string (key)] = std::make_pair (taken, total);
    }
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
      std::string base = (std::string (lbasename (xl.file)) + ":"
			  + std::to_string (xl.line) + ":"
			  + std::to_string (disc));

      edge e0 = EDGE_SUCC (bb, 0), e1 = EDGE_SUCC (bb, 1);
      std::string k0 = base + ">" + std::to_string (blp_bb_line (e0->dest));
      std::string k1 = base + ">" + std::to_string (blp_bb_line (e1->dest));
      std::map<std::string, blp_count>::iterator
	it0 = blp_map->find (k0),
	it1 = blp_map->find (k1),
	end = blp_map->end ();

      edge taken = NULL;
      unsigned long long t = 0, total = 0;
      if (it0 != end && it1 == end)
	{ taken = e0; t = it0->second.first; total = it0->second.second; }
      else if (it1 != end && it0 == end)
	{ taken = e1; t = it1->second.first; total = it1->second.second; }
      else
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
