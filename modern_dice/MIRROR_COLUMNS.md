# Partial mirror searches

`MIRROR_COLUMNS=K` forces physical columns `0..K-1` to mirror columns
`SIDES-K..SIDES-1`.  The `SIDES-2K` middle columns remain independent.
Consequently the search has `SIDES-K` independent column groups.

- `K=0` is the unrestricted column-grouped search.
- `K=SIDES/2` is the full mirror search.
- The legacy `MIRROR=1` setting remains an alias for full mirroring.

For example:

```sh
make pgo DICE=4 SIDES=18 PERM_ONLY=1 ROW_MITM=1 \
    COMPLETION_BOUNDS=1 MIRROR_COLUMNS=5
```

Place, linear-place, pair, additive-permutation, conditioned-completion, and
exact row-MITM checks all operate on the mixed paired/single column groups.
The early five-dice A/B completion DP remains safe for partial mirroring by
treating noncanonical mirror partners as independent.  This is a relaxation:
it may miss a prune, but it cannot reject a real partially mirrored solution.
The optional incremental C-completion bound uses the same relaxation.

## Exhaustive regression counts

These are canonical column-grouped permutation-fair solution counts.  The
subspaces are nested: every solution counted at `K+1` is also counted at `K`.

### 4d12

| K | Permutation-fair | Fully mirror-symmetric |
|--:|-----------------:|-----------------------:|
| 0 | 128 | 16 |
| 1 | 64 | 16 |
| 2 | 64 | 16 |
| 3 | 32 | 16 |
| 4 | 32 | 16 |
| 5 | 16 | 16 |
| 6 | 16 | 16 |

All seven counts were independently reproduced with the ordinary recursive
row search (`ROW_MITM=0 COMPLETION_BOUNDS=0`) and with both exact row joins
plus conditioned completion bounds.

### 4d18

| K | Permutation-fair | Fully mirror-symmetric |
|--:|-----------------:|-----------------------:|
| 0 | 3,828 | 456 |
| 1 | 2,032 | 456 |
| 2 | 1,732 | 456 |
| 3 | 684 | 456 |
| 4 | 580 | 456 |
| 5 | 480 | 456 |
| 6 | 480 | 456 |
| 7 | 480 | 456 |
| 8 | 456 | 456 |
| 9 | 456 | 456 |

The intermediate counts were independently obtained by saving all 3,828
encodings from one unrestricted search and classifying each encoding by its
longest mirrored outer prefix.  The `K=0` and `K=9` endpoints also match the
established unrestricted and full-mirror counts.  The equal `K=5..7` and
`K=8..9` counts are genuine plateaus, not rounding.
