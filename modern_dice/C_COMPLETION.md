# Incremental five-die C completion

`C_COMPLETION=1` enables an experimental bound for the `5d30`-style
permutation-only search:

```sh
make pgo DICE=5 SIDES=30 PERM_ONLY=1 ROW_MITM=1 \
    COMPLETION_BOUNDS=1 C_COMPLETION=1 C_COMPLETION_COUPLED=1 \
    MIRROR_COLUMNS=0
```

It requires five dice, permutation-only search, final-row MITM, and
conditioned completion bounds.  It is compile-time gated, so disabled builds
have no C-completion branches, tables, or per-worker storage.

After A and B have been built, C is traversed in physical column order.  For
each A/B-swapped five-die permutation identity, the remaining D/E choices can
be written as an additive recurrence with a scalar tally `F` and nonlinear
state `G`.  A backward table stores the minimum and maximum residual `F` for
every column and incoming `G`.  The C recursion maintains the matching
forward state without copying it.  A branch is pruned when zero is outside
the combined forward/residual interval for any checked identity.

The canonical column also fixes the remaining D/E order.  The number of
A/B-swapped suffix-order identities retained is selectable for measurement:

- `1` checks the cheap deterministic `CDE` direction and is the default.
- `2` also checks the cheap deterministic `CED` direction.
- `3` through `6` add `DCE`, `DEC`, `ECD`, and `EDC`, respectively; these
  require sparse nonlinear frontiers.

`C_COMPLETION_COUPLED=1` also checks `X+Y` and `X-Y`, where `X` and `Y` are
the CDE and CED directions.  Both directions have exactly the same nonlinear
`G` transition, so their coupled support bounds enforce the same C and D/E
choices without introducing a multidimensional state.  Coupled mode
therefore includes CED even when `C_COMPLETION_DIRECTIONS=1`; startup reports
the effective two base directions followed by `+X+Y/X-Y`.

This experiment is off by default.  Its first finite 5d30 benchmark showed
that the extra hot-loop state cost more than the additional pruning saved.
Leave `C_COMPLETION_COUPLED=0` for the fast independent-direction bound, or
opt in to test whether the tradeoff improves in a larger, less
mirror-constrained search.

The `c-coupled-pruned` status counter is the subset of
`c-completion-pruned` rejected specifically by one of these two support
bounds.

With partial mirroring, only already fixed canonical mirror cells constrain
the residual table.  Other mirror partners are deliberately treated as free,
which weakens the bound but preserves correctness.

Template searches currently retain their existing fail-first traversal and
do not invoke the incremental C bound.

## AB residual permutation fairness

Before either C strategy begins, five-die permutation-only builds apply the
compile-time `AB_RESIDUAL=1` check.  Every unbuilt face is collapsed into a
single `(DICE-2)*SIDES`-sided residual die `R`.  A fair completion makes every
`ABX` triple fair for `X` in the residual set, so summing their six ordering
counts requires `ABR` itself to be fair.  For 5d30 each ABR ordering must
therefore have

```text
3 * 30^3 / 3! = 13,500
```

outcomes.  This is an exact necessary condition depending only on completed
A and B; it is independent of how the residual faces are later partitioned
among C, D, and E.  It is checked before the conditioned early-completion DP
and reports `ab-residual-checks` and `ab-residual-pruned`.

With `ABR_BOUNDS=1` (the default), the same condition also prunes partial B
rows.  Although the residual die changes whenever a B face is selected, the
ABR counts are exactly additive by physical column in a column-grouped
configuration.  B owns one face in every column, so comparisons involving B
faces from different columns have a fixed order; the only variable endpoint
terms depend on one column at a time.  The search measures every candidate's
five independent ABR-coordinate differences from an arbitrary baseline and
builds suffix minima and maxima from those differences.  After each B choice,
it rejects the branch if any ABR goal lies outside the range reachable by the
remaining columns.  The sixth coordinate is omitted because the six counts
have a fixed sum.  The completed-row ABR check remains as an exact backstop,
and partial-row rejections are reported as `abr-bound-pruned`.

`ABR_ORDER=1` (the default) also uses those precomputed contributions when
planning B's column order.  The ordinary fail-first planner still sorts first
by the number of candidates that survive its place, linear-place, and pair
lookahead.  Among columns with the same surviving domain size, it prefers the
column with the largest total span across the five ABR coordinates.  Fixing a
high-span choice early removes more flexibility from the remaining ABR suffix
and lets the unchanged coordinate bounds reject contradictions sooner.  The
existing place-planner order remains the stable final tie-breaker.

Planning happens once after A is fixed and adds no work to recursive nodes.
On the unmirrored 5d30 benchmark, it raised completed AB rows after 120 seconds
from 649 to 1,793 (2.76x).  On the exhaustive `MIRROR_COLUMNS=13` case it
reduced 7.963G nodes and 9.63 seconds to 1.696G nodes and 2.05 seconds while
preserving all 1,012 AB survivors and every downstream count.  Setting
`ABR_ORDER_PRIMARY=1` makes ABR span the primary key, but measured slightly
slower than the default place-first hybrid.

`ABR_LINEAR=1` enables an additional coupled projection over the same
additive contributions.  The default five-direction basis compares each of
the five explicitly tracked ABR counts with the implicit sixth count, which
is recovered from their fixed total.  `ABR_LINEAR_FULL=1` instead checks all
15 pairwise differences.  By default these checks begin halfway through B
and run every third search depth; `ABR_LINEAR_START` and
`ABR_LINEAR_STRIDE` override that schedule.  Rejections are reported as
`linear-abr-pruned`.

This layer is disabled by default.  Its added pruning did not repay its
instruction cost either before or after ABR-aware ordering.  The compile-time
gate keeps all of its tables, counters, and hot-loop code out of production
builds while retaining it for larger or differently constrained searches
where the crossover may change.

## Exact C-row meet in the middle

`C_MITM=1` selects a separate exact C-row solver instead of recursive C
completion.  Once A and B are fixed, each physical-column choice for C is
additive in nine independent goal coordinates: five ABC permutation buckets
after omitting the fixed-sum sixth bucket, and four full-set place tallies
after omitting the fixed-sum fifth place.  The solver hashes every assignment
of one half of the ternary C choices and probes it with complementary
assignments from the other half.  Every fingerprint hit is reconstructed and
verified in all nine exact coordinates, so hash collisions cannot lose or
create configurations.

For an unmirrored 5d30 search the canonical column leaves 29 ternary choices,
reducing `3^29` possible C rows to `3^14 + 3^15` MITM states.  The table is
allocated once per worker before search and reused for every A/B row; there
is no allocation in the search loop.  Partial mirror searches naturally use
fewer independent columns.  Startup output reports both half-state counts and
the per-worker workspace size.

For example:

```sh
make pgo DICE=5 SIDES=30 PERM_ONLY=1 ROW_MITM=1 \
    COMPLETION_BOUNDS=1 C_MITM=1 MIRROR_COLUMNS=8
```

`C_MITM=1` requires five dice, permutation-only mode, and final-row MITM.
It is mutually exclusive with `C_COMPLETION=1`.  The early A/B completion
check and conditioned completion bounds still run when
`COMPLETION_BOUNDS=1`, and surviving exact C rows proceed to the existing
exact D-row MITM.  Template searches retain their ordinary recursive path.

## ABC residual permutation fairness

After C is complete, five-die permutation-only builds apply the independent
compile-time `ABC_RESIDUAL=1` check.  The unbuilt D and E faces are collapsed
into one `2*SIDES`-sided residual die `R`.  Fair ABCD and ABCE subsets make
each ABCR ordering the sum of two equal four-die ordering counts, so for
5d30 each of the 24 orderings must have

```text
2 * 30^4 / 4! = 67,500
```

outcomes.  The exact check uses `O(SIDES^3 + FACE_COUNT)` work, performs no
allocation, and runs before the conditioned D/E completion check and D-row
MITM.  It reports `abc-residual-checks` and `abc-residual-pruned`.  C-MITM
matches count exact ABC rows before this additional theorem is applied.

## Row progress funnel

Progress and final summaries report `rows=A:...,B:...,C:...`.  A nonfinal
row increments only after it has passed the applicable fairness and
completion checks and search is about to enter the next row.  Thus with
early completion enabled, `B` equals
`early-completion-checks - early-completion-pruned`; in C-MITM mode it also
equals `c-mitm-solves`, while `C` equals
`c-mitm-matches - abc-residual-pruned`.  The final row is forced and
increments when a complete configuration reaches final verification, before
the stronger permutation-fair test.  Fully prebuilt template rows are
initialization rather than search work and are not counted.

## Optional row CPU-time attribution

Build with `TRACK_TIME=1` to add a `time=A:...,B:...` field to progress and
final summaries.  The percentages are exclusive thread CPU time summed over
all workers: a parent row is charged until it enters a child row, pauses while
the child runs, and resumes when the child returns.  Consequently a failed B
subtree is charged to B rather than A, C-MITM work is charged to C, D-MITM
work is charged to D, and forced final verification is charged to the last
die.

Only completed timing intervals are published.  A worker spending a long
time in one uninterrupted row will not expose that interval to a periodic
status update until it changes rows or finishes its prefix.  This avoids
watcher-side access to mutable timer state.  The timers use
`CLOCK_THREAD_CPUTIME_ID`, so scheduling delays are excluded and total
accumulated time grows at roughly one second per busy worker per wall-clock
second.

`TRACK_TIME=0` is the default and compiles out the timer state, clock reads,
row-transition branches, atomic counters, formatting, and reporting.

## Independent validation

`c_completion_oracle.py` independently translates the recurrence and
exhaustively enumerates every D/E completion for small random instances,
including the fixed canonical C/D positions.  For example:

```sh
python3 c_completion_oracle.py --sides 4 --trials 500
python3 c_completion_oracle.py --sides 5 --trials 100 --mirror-columns 2
```

The production feature remains opt-in.  Short `5d30` measurements show that
all six directions can roughly halve the visited nodes at `K=0`, but their
sparse-frontier maintenance also roughly halves nodes per second.  The cheap
one-direction default removes less work at much lower cost.  Longer,
workload-representative measurements are needed before enabling C completion
by default.
