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
