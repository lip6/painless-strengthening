#include "internal.hpp"

namespace CaDiCaL {

// The global assignment stack can only be (partially) reset through
// 'backtrack' which is the only function using 'unassign' (inlined and thus
// local to this file).  It turns out that 'unassign' does not need a
// specialization for 'probe' nor 'vivify' and thus it is shared.

inline void Internal::unassign (int lit) {
  assert (val (lit) > 0);
  set_val (lit, 0);

  int idx = vidx (lit);
  LOG ("unassign %d @ %d", lit, var (idx).level);
  num_assigned--;

  // In the standard EVSIDS variable decision heuristic of MiniSAT, we need
  // to push variables which become unassigned back to the heap.
  //
  if (!scores.contains (idx))
    scores.push_back (idx);

  // For VMTF we need to update the 'queue.unassigned' pointer in case this
  // variable sits after the variable to which 'queue.unassigned' currently
  // points.  See our SAT'15 paper for more details on this aspect.
  //
  if (queue.bumped < btab[idx])
    update_queue_unassigned (idx);
}

/*------------------------------------------------------------------------*/

// Update the current target maximum assignment and also the very best
// assignment.  Whether a trail produces a conflict is determined during
// propagation.  Thus that all functions in the 'search' loop after
// propagation can assume that 'no_conflict_until' is valid.  If a conflict
// is found then the trail before the last decision is used (see the end of
// 'propagate').  During backtracking we can then save this largest
// propagation conflict free assignment.  It is saved as both 'target'
// assignment for picking decisions in 'stable' mode and if it is the
// largest ever such assignment also as 'best' assignment. This 'best'
// assignment can then be used in future stable decisions after the next
// 'rephase_best' overwrites saved phases with it.

void Internal::update_target_and_best () {

  bool reset = (rephased && stats.conflicts > last.rephase.conflicts);

  if (reset) {
    target_assigned = 0;
    if (rephased == 'B')
      best_assigned = 0; // update it again
  }

  if (no_conflict_until > target_assigned) {
    copy_phases (phases.target);
    target_assigned = no_conflict_until;
    LOG ("new target trail level %zu", target_assigned);
  }

  if (no_conflict_until > best_assigned) {
    copy_phases (phases.best);
    best_assigned = no_conflict_until;
    LOG ("new best trail level %zu", best_assigned);
  }

  if (reset) {
    report (rephased);
    rephased = 0;
  }
}

/*------------------------------------------------------------------------*/

void Internal::backtrack (int new_level) {

  assert (new_level <= level);
  if (new_level == level)
    return;

  stats.backtracks++;
  update_target_and_best ();

  // sometimes we do not use multitrail even with option enabled e.g. in
  // probe
  if (opts.reimply && !trails.empty ()) {
    multi_backtrack (new_level);
    return;
  }

  assert (num_assigned == trail.size ());

  const size_t assigned = control[new_level + 1].trail;

  LOG ("backtracking to decision level %d with decision %d and trail %zd",
       new_level, control[new_level].decision, assigned);

  const size_t end_of_trail = trail.size ();
  size_t i = assigned, j = i;

#ifdef LOGGING
  int unassigned = 0;
#endif
  int reassigned = 0;

  notify_backtrack (new_level);
  if (external_prop && !external_prop_is_lazy && notified > assigned) {
    LOG ("external propagator is notified about some unassignments (trail: "
         "%zd, notified: %zd).",
         trail.size (), notified);
    notified = assigned;
  }

  while (i < end_of_trail) {
    int lit = trail[i++];
    Var &v = var (lit);
    if (v.level > new_level) {
      unassign (lit);
#ifdef LOGGING
      unassigned++;
#endif
    } else {
      // This is the essence of the SAT'18 paper on chronological
      // backtracking.  It is possible to just keep out-of-order assigned
      // literals on the trail without breaking the solver (after some
      // modifications to 'analyze' - see 'opts.chrono' guarded code there).
      assert (opts.chrono || external_prop || did_external_prop);
#ifdef LOGGING
      if (!v.level)
        LOG ("reassign %d @ 0 unit clause %d", lit, lit);
      else
        LOG (v.reason, "reassign %d @ %d", lit, v.level);
#endif
      trail[j] = lit;
      v.trail = j++;
#ifdef LOGGING
      reassigned++;
#endif
    }
  }
  trail.resize (j);
  LOG ("unassigned %d literals %.0f%%", unassigned,
       percent (unassigned, unassigned + reassigned));
  LOG ("reassigned %d literals %.0f%%", reassigned,
       percent (reassigned, unassigned + reassigned));

  if (propagated > assigned)
    propagated = assigned;
  if (propagated2 > assigned)
    propagated2 = assigned;
  if (no_conflict_until > assigned)
    no_conflict_until = assigned;

  propergated = 0; // Always go back to root-level.

  assert (notified <= assigned + reassigned);
  if (reassigned) {
    notify_assignments ();
  }

  control.resize (new_level + 1);
  level = new_level;
  if (tainted_literal) {
    assert (opts.ilb);
    if (!val (tainted_literal)) {
      tainted_literal = 0;
    }
  }
  assert (num_assigned == trail.size ());
}

void Internal::multi_backtrack (int new_level) {

  assert (opts.reimply);
  assert (0 <= new_level && new_level < level);

  LOG ("backtracking on multitrail to decision level %d with decision %d",
       new_level, control[new_level].decision);

  int elevated = 0, unassigned = 0;

  for (int i = new_level; i < level; i++) {
    assert (level > 0);
    assert (i >= 0); // check that loop is safe for level = INT_MAX
    int l = i + 1;
    LOG ("unassigning level %d", l);
    auto &t = trails[i];
    for (auto &lit : t) {
      LOG ("unassigning literal %d", lit);
      if (!lit) { // cannot happen
        assert (false);
        LOG ("empty space on trail level %d", l);
        elevated++;
        continue;
      }
      Var &v = var (lit);
      if (v.level == l) {
        unassign (lit);
        unassigned++;
      } else {
        // after intelsat paper from 2022
        LOG ("elevated literal %d on level %d", lit, v.level);
        // assert (opts.chrono); probably not true anymore...
        elevated++;
      }
    }
  }

  LOG ("unassigned %d literals %.0f%%", unassigned,
       percent (unassigned, unassigned + elevated));
  LOG ("elevated %d literals %.0f%%", elevated,
       percent (elevated, unassigned + elevated));
#ifndef LOGGING
  (void) unassigned;
#endif

  stats.elevated += elevated;

  if (external_prop && !external_prop_is_lazy) {
    notify_backtrack (new_level);
    notified = control[new_level + 1].trail;
    assert (notified <= notify_trail.size ());
    LOG ("external propagator is notified about some unassignments (trail: "
         "%zd, notified: %zd).",
         notify_trail.size (), notified);
    size_t i = notified, j = i;
    const size_t eot = notify_trail.size ();
    while (i != eot) {
      assert (i < eot);
      const int lit = notify_trail[i++];
      const signed char tmp = val (lit);
      assert (tmp >= 0);
      if (!tmp)
        continue;
      notify_trail[j++] = lit;
    }
    notify_trail.resize (j);
    notify_assignments ();
  } else {
    notify_trail.clear ();
  }

  assert (new_level >= 0);
  if (multitrail_dirty > new_level)
    multitrail_dirty = new_level;
  propergated = 0; // Always go back to root-level.
  clear_trails (new_level);
  multitrail.resize (new_level);
  control.resize (new_level + 1);
  level = new_level;
  if (tainted_literal) {
    assert (opts.ilb);
    if (!val (tainted_literal)) {
      tainted_literal = 0;
    }
  }
}

} // namespace CaDiCaL