#!/usr/bin/env python3
"""Derive exact identities for column-grouped Go First Dice.

For a roll order ``word``, the selected faces have nondecreasing physical
column numbers.  Runs of equal column numbers are consecutive blocks of the
word, so every roll belongs to exactly one composition of ``len(word)``.
This gives the exact collision expansion

    F[word] = sum(compositions alpha)
                  sum(c_1 < ... < c_r)
                      product_j I(c_j, block_j(word)),

where I(c, block) says that the dice in ``block`` occur in that order inside
column c.  Singleton blocks have I == 1 and are summed into binomial gap
weights.  The resulting aggregate features are linear through three dice,
quadratic through five dice, and first become cubic at six dice.

This program uses exact SymPy linear algebra to project the fair permutation
equations into:

  * full-set place-fair directions;
  * all-subset-place-fair directions; and
  * remaining permutation-only directions.

It is a theory and validation tool, not a searcher.  Its output is intended
to identify small identities that are worth implementing as incremental
bounds in column_search.c.  It also reports the exact additive dimension and
balanced meet-in-the-middle cost of completing each independently built row.
"""

from __future__ import annotations

import argparse
import itertools
import json
import math
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Iterable, Iterator, Mapping, Sequence

try:
    import sympy
except ImportError as error:  # pragma: no cover - environment diagnostic
    print(
        "error: column_theory.py requires SymPy",
        file=sys.stderr,
    )
    raise SystemExit(2) from error


Word = tuple[int, ...]
Direction = tuple[int, ...]


@dataclass(frozen=True, order=True)
class Feature:
    """A column-collision aggregate after summing singleton blocks.

    ``blocks`` initially contains the nonsingleton ordered blocks. ``gaps``
    contains the number of unconstrained selected columns before, between,
    and after them.  Before canonicalization those unconstrained columns are
    precisely the singleton blocks.  Eliminating a within-column indicator
    with the one-hot column-permutation identity turns its column into
    another unconstrained gap marker.  Thus

        Feature(((A, B),), (1, 1))

    denotes

        sum_c c * (m - 1 - c) * I_c(AB).
    """

    blocks: tuple[Word, ...]
    gaps: tuple[int, ...]

    def __post_init__(self) -> None:
        if len(self.gaps) != len(self.blocks) + 1:
            raise ValueError("a feature needs one more gap than block")
        if any(len(block) < 2 for block in self.blocks):
            raise ValueError("feature blocks must be nonsingletons")
        if any(gap < 0 for gap in self.gaps):
            raise ValueError("feature gaps must be nonnegative")

    @property
    def order(self) -> int:
        return sum(map(len, self.blocks)) + sum(self.gaps)

    @property
    def correlation_degree(self) -> int:
        return len(self.blocks)


@dataclass(frozen=True)
class NamedDirection:
    name: str
    coefficients: Direction


@dataclass
class OrderTheory:
    order: int
    permutations: list[Word]
    raw_expansions: list[dict[Feature, int]]
    expansions: list[dict[Feature, int]]
    within_column_canonicalized: bool
    full_place_basis: list[NamedDirection]
    all_subset_place_basis: list[NamedDirection]
    permutation_only_basis: list[NamedDirection]
    single_column_fair_rank: int | None
    all_subset_spans_single_column: bool | None


class ExactRowSpace:
    """Incremental exact row space for small integer theory vectors."""

    def __init__(self, width: int) -> None:
        self.width = width
        self.rows: dict[int, list[Fraction]] = {}

    @property
    def rank(self) -> int:
        return len(self.rows)

    def add(self, values: Sequence[int]) -> bool:
        if len(values) != self.width:
            raise ValueError("row-space vector has the wrong width")
        vector = [Fraction(value) for value in values]
        for pivot in sorted(self.rows):
            factor = vector[pivot]
            if factor:
                row = self.rows[pivot]
                for column in range(pivot, self.width):
                    vector[column] -= factor * row[column]
        pivot = next(
            (index for index, value in enumerate(vector) if value), None
        )
        if pivot is None:
            return False
        divisor = vector[pivot]
        for column in range(pivot, self.width):
            vector[column] /= divisor
        self.rows[pivot] = vector
        return True


CONFIGURATION_RE = re.compile(
    r"^(?P<kind>permutation-fair|all-subset-place-fair)\s+"
    r"config=(?P<dice>\d+)d(?P<sides>\d+).*?"
    r"encoding=(?P<encoding>[A-Za-z]+)\s*$"
)


def positive_int(text: str) -> int:
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return value


def nonnegative_int(text: str) -> int:
    value = int(text)
    if value < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    return value


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Derive exact column-collision identities and separate their "
            "place-fair and permutation-only components."
        )
    )
    parser.add_argument("--dice", "-d", type=positive_int, default=4)
    parser.add_argument("--sides", "-s", type=positive_int, default=18)
    parser.add_argument(
        "--max-order",
        type=positive_int,
        help="derive through this subset size (default: DICE)",
    )
    parser.add_argument(
        "--show-identities",
        type=nonnegative_int,
        default=3,
        metavar="N",
        help=(
            "show the N shortest identities in each family and order "
            "(default: 3; zero shows summaries only)"
        ),
    )
    parser.add_argument(
        "--print-term-limit",
        type=positive_int,
        default=16,
        metavar="N",
        help="print at most N terms from a long identity (default: 16)",
    )
    parser.add_argument(
        "--solutions-file",
        type=Path,
        help="validate identities against recorded column-search solutions",
    )
    parser.add_argument(
        "--validate-limit",
        type=nonnegative_int,
        default=16,
        metavar="N",
        help=(
            "validate at most N encodings of each recorded kind "
            "(default: 16; zero validates every encoding)"
        ),
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="also verify every expansion on deterministic unfair examples",
    )
    parser.add_argument(
        "--json",
        type=Path,
        help="write every derived basis and expanded identity as JSON",
    )
    return parser.parse_args()


def owner_name(owner: int) -> str:
    if owner < 26:
        return chr(ord("A") + owner)
    return f"D{owner}"


def word_name(word: Sequence[int]) -> str:
    return "".join(owner_name(owner) for owner in word)


def compositions(total: int) -> Iterator[tuple[int, ...]]:
    """Yield all ordered positive compositions of total."""

    if total <= 0:
        return
    for cut_mask in range(1 << (total - 1)):
        result: list[int] = []
        previous = 0
        for boundary in range(1, total):
            if cut_mask & (1 << (boundary - 1)):
                result.append(boundary - previous)
                previous = boundary
        result.append(total - previous)
        yield tuple(result)


def feature_for_composition(word: Word, sizes: Sequence[int]) -> Feature:
    blocks: list[Word] = []
    gaps = [0]
    offset = 0
    for size in sizes:
        block = word[offset : offset + size]
        offset += size
        if size == 1:
            gaps[-1] += 1
        else:
            blocks.append(block)
            gaps.append(0)
    if offset != len(word):
        raise ValueError("composition does not cover its word")
    return Feature(tuple(blocks), tuple(gaps))


def collision_expansion(word: Word) -> dict[Feature, int]:
    expansion: dict[Feature, int] = defaultdict(int)
    for sizes in compositions(len(word)):
        expansion[feature_for_composition(word, sizes)] += 1
    return dict(expansion)


def block_in_full_permutation(full: Word, block: Word) -> bool:
    positions = {owner: position for position, owner in enumerate(full)}
    return block_matches(positions, block)


def canonical_block_expansion(
    block: Word,
    full_permutations: Sequence[Word],
) -> list[tuple[Word | None, int]]:
    """Express I(block) in one-hot full-column permutation coordinates.

    The lexicographically first full permutation is eliminated.  ``None``
    is its constant contribution and means that this selected physical
    column remains present but its ordering is unconstrained.
    """

    reference_matches = block_in_full_permutation(full_permutations[0], block)
    result: list[tuple[Word | None, int]] = []
    if reference_matches:
        result.append((None, 1))
    for full in full_permutations[1:]:
        matches = block_in_full_permutation(full, block)
        coefficient = int(matches) - int(reference_matches)
        if coefficient:
            result.append((full, coefficient))
    return result


def canonicalize_feature(
    feature: Feature,
    full_permutations: Sequence[Word],
) -> dict[Feature, int]:
    if not feature.blocks:
        return {feature: 1}

    choices = [
        canonical_block_expansion(block, full_permutations)
        for block in feature.blocks
    ]
    result: dict[Feature, int] = defaultdict(int)
    for selected in itertools.product(*choices):
        coefficient = math.prod(item[1] for item in selected)
        blocks: list[Word] = []
        gaps = [feature.gaps[0]]
        for index, (block, _) in enumerate(selected):
            if block is None:
                gaps[-1] += 1 + feature.gaps[index + 1]
            else:
                blocks.append(block)
                gaps.append(feature.gaps[index + 1])
        result[Feature(tuple(blocks), tuple(gaps))] += coefficient
    return {
        item: coefficient for item, coefficient in result.items() if coefficient
    }


def canonicalize_expansion(
    expansion: Mapping[Feature, int],
    full_permutations: Sequence[Word],
) -> dict[Feature, int]:
    result: dict[Feature, int] = defaultdict(int)
    for feature, outer_coefficient in expansion.items():
        for canonical, inner_coefficient in canonicalize_feature(
            feature, full_permutations
        ).items():
            result[canonical] += outer_coefficient * inner_coefficient
    return {
        item: coefficient for item, coefficient in result.items() if coefficient
    }


def column_positions(columns: Sequence[Word]) -> list[dict[int, int]]:
    return [
        {owner: position for position, owner in enumerate(column)}
        for column in columns
    ]


def block_matches(positions: Mapping[int, int], block: Word) -> bool:
    return all(
        positions[first] < positions[second]
        for first, second in zip(block, block[1:])
    )


def evaluate_feature(
    feature: Feature,
    positions: Sequence[Mapping[int, int]],
) -> int:
    sides = len(positions)
    if not feature.blocks:
        return math.comb(sides, feature.gaps[0])

    total = 0
    for selected in itertools.combinations(
        range(sides), len(feature.blocks)
    ):
        if not all(
            block_matches(positions[column], block)
            for column, block in zip(selected, feature.blocks)
        ):
            continue

        ways = math.comb(selected[0], feature.gaps[0])
        for index in range(1, len(selected)):
            available = selected[index] - selected[index - 1] - 1
            ways *= math.comb(available, feature.gaps[index])
        ways *= math.comb(
            sides - selected[-1] - 1, feature.gaps[-1]
        )
        total += ways
    return total


def evaluate_expansion(
    expansion: Mapping[Feature, int],
    values: Mapping[Feature, int],
) -> int:
    return sum(coefficient * values[feature]
               for feature, coefficient in expansion.items())


def permutation_subsequence_counts(
    encoding: Sequence[int], dice: int
) -> dict[Word, int]:
    """Count every distinct-die subsequence by the searcher's DP rule."""

    states = [
        state
        for length in range(1, dice + 1)
        for state in itertools.permutations(range(dice), length)
    ]
    ways: dict[Word, int] = {(): 1}
    ways.update((state, 0) for state in states)
    ending_at: list[list[Word]] = [[] for _ in range(dice)]
    for state in states:
        ending_at[state[-1]].append(state)
    for owner in range(dice):
        ending_at[owner].sort(key=len, reverse=True)

    for owner in encoding:
        for state in ending_at[owner]:
            ways[state] += ways[state[:-1]]
    ways.pop(())
    return ways


def relative_position(permutation: Word, subset: frozenset[int], die: int) -> int:
    return sum(
        1
        for owner in permutation[: permutation.index(die)]
        if owner in subset
    )


def place_direction_candidates(
    order: int,
    permutations: Sequence[Word],
    subset_sizes: Iterable[int],
) -> list[NamedDirection]:
    labels = range(order)
    result: list[NamedDirection] = []
    for size in subset_sizes:
        for members_tuple in itertools.combinations(labels, size):
            members = frozenset(members_tuple)
            subset_label = word_name(members_tuple)
            for die in members_tuple:
                for place in range(1, size):
                    coefficients = tuple(
                        (
                            1
                            if relative_position(permutation, members, die)
                            == place
                            else -1
                            if relative_position(permutation, members, die)
                            == 0
                            else 0
                        )
                        for permutation in permutations
                    )
                    result.append(
                        NamedDirection(
                            f"place[{subset_label},{owner_name(die)},"
                            f"{place}-0]",
                            coefficients,
                        )
                    )
    return result


def vector_rank(vectors: Sequence[Direction], width: int) -> int:
    space = ExactRowSpace(width)
    for vector in vectors:
        space.add(vector)
    return space.rank


def extend_independent_basis(
    candidates: Iterable[NamedDirection],
    width: int,
    initial: Sequence[NamedDirection] = (),
) -> list[NamedDirection]:
    """Greedily select candidates independent modulo initial."""

    space = ExactRowSpace(width)
    for item in initial:
        space.add(item.coefficients)
    selected: list[NamedDirection] = []
    for candidate in candidates:
        if space.add(candidate.coefficients):
            selected.append(candidate)
    return selected


def simple_fairness_directions(
    permutations: Sequence[Word],
) -> Iterator[NamedDirection]:
    reference = 0
    for index in range(1, len(permutations)):
        coefficients = [0] * len(permutations)
        coefficients[index] = 1
        coefficients[reference] = -1
        yield NamedDirection(
            f"F[{word_name(permutations[index])}]"
            f"-F[{word_name(permutations[reference])}]",
            tuple(coefficients),
        )


def combine_expansions(
    coefficients: Direction,
    expansions: Sequence[Mapping[Feature, int]],
) -> dict[Feature, int]:
    result: dict[Feature, int] = defaultdict(int)
    for direction_coefficient, expansion in zip(coefficients, expansions):
        if direction_coefficient == 0:
            continue
        for feature, feature_coefficient in expansion.items():
            result[feature] += direction_coefficient * feature_coefficient
    return {
        feature: coefficient
        for feature, coefficient in result.items()
        if coefficient
    }


def identity_cost(identity: Mapping[Feature, int]) -> tuple[int, int, int]:
    return (
        max(
            (feature.correlation_degree for feature in identity),
            default=0,
        ),
        len(identity),
        sum(abs(coefficient) for coefficient in identity.values()),
    )


def derive_order_theory(order: int) -> OrderTheory:
    permutations = list(itertools.permutations(range(order)))
    raw_expansions = [collision_expansion(word) for word in permutations]
    # Expanding every restriction into a common full-column permutation
    # basis gives an exact test for cancellation of correlation terms.  Its
    # factorial growth is acceptable through four dice.  The raw collision
    # basis remains exact at higher orders and avoids a theory-tool blowup.
    within_column_canonicalized = order <= 4
    expansions = (
        [
            canonicalize_expansion(expansion, permutations)
            for expansion in raw_expansions
        ]
        if within_column_canonicalized
        else raw_expansions
    )
    width = len(permutations)

    def candidate_cost(item: NamedDirection) -> tuple[int, int, int, int, str]:
        canonical = combine_expansions(item.coefficients, expansions)
        raw = combine_expansions(item.coefficients, raw_expansions)
        return (
            identity_cost(canonical)[0],
            identity_cost(raw)[1],
            identity_cost(raw)[2],
            identity_cost(canonical)[1],
            item.name,
        )

    full_candidates = place_direction_candidates(
        order, permutations, [order]
    )
    full_candidates.sort(key=candidate_cost)
    full_place_basis = extend_independent_basis(full_candidates, width)

    all_subset_candidates = place_direction_candidates(
        order, permutations, range(2, order + 1)
    )
    all_subset_candidates.sort(key=candidate_cost)
    all_subset_place_basis = extend_independent_basis(
        all_subset_candidates, width
    )

    permutation_candidates = list(simple_fairness_directions(permutations))
    permutation_candidates.sort(key=candidate_cost)
    permutation_only_basis = extend_independent_basis(
        permutation_candidates,
        width,
        initial=all_subset_place_basis,
    )
    expected_dimension = math.factorial(order) - 1
    combined_rank = vector_rank(
        [
            item.coefficients
            for item in all_subset_place_basis + permutation_only_basis
        ],
        width,
    )
    if combined_rank != expected_dimension:
        raise RuntimeError(
            f"order {order}: derived rank {combined_rank}, "
            f"expected {expected_dimension}"
        )

    single_column_fair_rank: int | None = None
    all_subset_spans_single_column: bool | None = None
    if within_column_canonicalized:
        quadratic_features = sorted(
            {
                feature
                for expansion in expansions
                for feature in expansion
                if feature.correlation_degree >= 2
            }
        )
        # A direction in permutation-count space has a purely single-column
        # collision identity exactly when every quadratic feature cancels.
        # The final all-ones row restricts us to fairness deviations whose
        # target is zero rather than changing the common bucket total.
        cancellation_rows = [
            [expansion.get(feature, 0) for expansion in expansions]
            for feature in quadratic_features
        ]
        cancellation_rows.append([1] * width)
        cancellation_rank = int(sympy.Matrix(cancellation_rows).rank())
        single_column_fair_rank = width - cancellation_rank
        all_subset_identities_are_single_column = all(
            identity_cost(
                combine_expansions(item.coefficients, expansions)
            )[0]
            <= 1
            for item in all_subset_place_basis
        )
        all_subset_spans_single_column = (
            all_subset_identities_are_single_column
            and len(all_subset_place_basis) == single_column_fair_rank
        )

    return OrderTheory(
        order=order,
        permutations=permutations,
        raw_expansions=raw_expansions,
        expansions=expansions,
        within_column_canonicalized=within_column_canonicalized,
        full_place_basis=full_place_basis,
        all_subset_place_basis=all_subset_place_basis,
        permutation_only_basis=permutation_only_basis,
        single_column_fair_rank=single_column_fair_rank,
        all_subset_spans_single_column=all_subset_spans_single_column,
    )


def format_binomial(argument: str, count: int) -> str:
    if count == 0:
        return "1"
    if count == 1:
        return argument
    return f"C({argument},{count})"


def format_feature(feature: Feature) -> str:
    if not feature.blocks:
        return f"C(m,{feature.gaps[0]})"

    columns = ["c", "d", "e", "f", "g", "h"]
    factors: list[str] = []
    first = columns[0]
    factors.append(format_binomial(first, feature.gaps[0]))
    for index in range(1, len(feature.blocks)):
        previous = columns[index - 1]
        current = columns[index]
        factors.append(
            format_binomial(
                f"{current}-{previous}-1", feature.gaps[index]
            )
        )
    last = columns[len(feature.blocks) - 1]
    factors.append(
        format_binomial(f"m-1-{last}", feature.gaps[-1])
    )
    factors = [factor for factor in factors if factor != "1"]
    for index, block in enumerate(feature.blocks):
        factors.append(f"I_{columns[index]}({word_name(block)})")
    body = " ".join(factors) if factors else "1"
    indices = "<".join(columns[: len(feature.blocks)])
    return f"sum_{{{indices}}} {body}"


def format_identity(
    identity: Mapping[Feature, int], term_limit: int | None = None
) -> str:
    pieces: list[str] = []
    ordered = sorted(
        identity.items(), key=lambda item: (item[0].correlation_degree, item[0])
    )
    omitted = 0
    if term_limit is not None and len(ordered) > term_limit:
        omitted = len(ordered) - term_limit
        ordered = ordered[:term_limit]
    for feature, coefficient in ordered:
        magnitude = abs(coefficient)
        body = format_feature(feature)
        if magnitude != 1:
            body = f"{magnitude}*({body})"
        if not pieces:
            pieces.append(body if coefficient > 0 else f"-{body}")
        else:
            pieces.append((" + " if coefficient > 0 else " - ") + body)
    if omitted:
        pieces.append(f" + ... [{omitted} more terms]")
    return "".join(pieces) + " = 0"


def direction_identity(
    theory: OrderTheory, direction: NamedDirection
) -> dict[Feature, int]:
    return combine_expansions(direction.coefficients, theory.expansions)


def print_direction_family(
    title: str,
    theory: OrderTheory,
    directions: Sequence[NamedDirection],
    show_count: int,
    term_limit: int,
) -> None:
    identities = []
    for item in directions:
        canonical = direction_identity(theory, item)
        raw = combine_expansions(item.coefficients, theory.raw_expansions)
        identities.append(
            (identity_cost(canonical), identity_cost(raw), item, raw)
        )
    identities.sort(key=lambda item: (item[0][0], item[1], item[2].name))
    costs: dict[int, int] = defaultdict(int)
    for canonical_cost, _, _, _ in identities:
        costs[canonical_cost[0]] += 1
    degree_summary = ", ".join(
        f"degree-{degree}:{count}" for degree, count in sorted(costs.items())
    ) or "none"
    print(f"  {title}: rank={len(directions)}; {degree_summary}")
    for canonical_cost, raw_cost, direction, identity in identities[:show_count]:
        print(
            f"    {direction.name}: raw-terms={raw_cost[1]}, "
            f"raw-L1={raw_cost[2]}, reduced-degree={canonical_cost[0]}"
        )
        print(f"      {format_identity(identity, term_limit)}")


def print_collision_example(theory: OrderTheory) -> None:
    word = theory.permutations[0]
    expansion = theory.raw_expansions[0]
    target = f"m^{theory.order}/{theory.order}!"
    pieces = [format_feature(feature) for feature in sorted(expansion)]
    print(f"  exact F[{word_name(word)}] collision expansion:")
    print("    " + " +\n      ".join(pieces) + f" = {target}")
    if theory.order == 2:
        print("  reduced pair theorem: sum_c I_c(XY) = m/2")
    elif theory.order == 3:
        print(
            "  reduced triple theorem after pair balance: "
            "N[XYZ] - M[XY] + M[YZ] = m/6"
        )
        print(
            "    M[XY]=sum_c c*I_c(XY), "
            "N[XYZ]=sum_c I_c(XYZ); hence N[XYZ]=N[ZYX]"
        )


def place_contribution(face: int, dice: int, sides: int) -> tuple[int, ...]:
    column, offset = divmod(face, dice)
    polynomial = [1]
    for opponent in range(dice - 1):
        if opponent < offset:
            below = column + 1
            above = sides - column - 1
        else:
            below = column
            above = sides - column
        following = [0] * (len(polynomial) + 1)
        for place, count in enumerate(polynomial):
            following[place] += count * below
            following[place + 1] += count * above
        polynomial = following
    return tuple(polynomial)


def normalized_integer_equation(
    coefficients: Sequence[sympy.Expr], rhs: sympy.Expr
) -> tuple[tuple[int, ...], int]:
    values = list(coefficients) + [rhs]
    denominator = math.lcm(
        *(int(sympy.denom(value)) for value in values)
    )
    integers = [int(value * denominator) for value in values]
    divisor = math.gcd(*map(abs, integers))
    if divisor:
        integers = [value // divisor for value in integers]
    first_nonzero = next((value for value in integers if value), 0)
    if first_nonzero < 0:
        integers = [-value for value in integers]
    return tuple(integers[:-1]), integers[-1]


def derive_place_moment_equations(
    dice: int, sides: int
) -> tuple[list[tuple[int, int]], list[tuple[tuple[int, ...], int]]]:
    """Return a compact moment form exactly equivalent to place fairness.

    If a die occupies offset r_c in physical column c, define

        S[j,q] = sum_c binomial(r_c,j) * c**q.

    The face contribution generating polynomial factors through these
    moments.  RREF leaves DICE-1 independent affine equations.
    """

    x = sympy.symbols("x")
    variables = [
        (j, q)
        for j in range(1, dice)
        for q in range(dice - j)
    ]
    symbols = {
        item: sympy.symbols(f"S_{item[0]}_{item[1]}")
        for item in variables
    }

    # Offset zero supplies the assignment-independent baseline B_c(x)^(D-1).
    expression = sum(
        (column + (sides - column) * x) ** (dice - 1)
        for column in range(sides)
    )
    for j, q in variables:
        expression += (
            (1 - x) ** (j + q)
            * sympy.binomial(dice - 1 - j, q)
            * (sides * x) ** (dice - 1 - j - q)
            * symbols[j, q]
        )

    target_numerator = sides**dice
    if target_numerator % dice:
        raise ValueError(
            f"{dice}d{sides} has a nonintegral place target"
        )
    target = target_numerator // dice * sum(x**place for place in range(dice))
    difference = sympy.Poly(sympy.expand(expression - target), x)
    zero_substitution = {symbol: 0 for symbol in symbols.values()}
    augmented_rows = []
    for place in range(dice):
        coefficient = sympy.expand(difference.coeff_monomial(x**place))
        augmented_rows.append(
            [coefficient.coeff(symbol) for symbol in symbols.values()]
            + [-coefficient.subs(zero_substitution)]
        )

    reduced, _ = sympy.Matrix(augmented_rows).rref()
    equations: list[tuple[tuple[int, ...], int]] = []
    for row in range(reduced.rows):
        coefficients = list(reduced.row(row)[:-1])
        rhs = reduced[row, reduced.cols - 1]
        if not any(coefficients):
            if rhs:
                raise RuntimeError("place moment system is inconsistent")
            continue
        equations.append(normalized_integer_equation(coefficients, rhs))
    if len(equations) != dice - 1:
        raise RuntimeError(
            f"place moment system has rank {len(equations)}, expected {dice - 1}"
        )
    return variables, equations


def format_linear_equation(
    variables: Sequence[tuple[int, int]],
    coefficients: Sequence[int],
    rhs: int,
) -> str:
    pieces: list[str] = []
    for variable, coefficient in zip(variables, coefficients):
        if coefficient == 0:
            continue
        magnitude = abs(coefficient)
        term = f"S[{variable[0]},{variable[1]}]"
        if magnitude != 1:
            term = f"{magnitude}*{term}"
        if not pieces:
            pieces.append(term if coefficient > 0 else f"-{term}")
        else:
            pieces.append((" + " if coefficient > 0 else " - ") + term)
    return "".join(pieces) + f" = {rhs}"


def place_moment_values(
    columns: Sequence[Word], die: int, variables: Sequence[tuple[int, int]]
) -> tuple[int, ...]:
    return tuple(
        sum(
            math.comb(column.index(die), j) * physical_column**q
            for physical_column, column in enumerate(columns)
        )
        for j, q in variables
    )


def place_moment_equations_hold(
    columns: Sequence[Word],
    variables: Sequence[tuple[int, int]],
    equations: Sequence[tuple[Sequence[int], int]],
) -> bool:
    dice = len(columns[0])
    for die in range(dice):
        values = place_moment_values(columns, die, variables)
        for coefficients, rhs in equations:
            if sum(
                coefficient * value
                for coefficient, value in zip(coefficients, values)
            ) != rhs:
                return False
    return True


def direct_full_place_fair(
    columns: Sequence[Word], dice: int, sides: int
) -> bool:
    goal = sides**dice // dice
    for die in range(dice):
        tally = [0] * dice
        for column, permutation in enumerate(columns):
            face = column * dice + permutation.index(die)
            contribution = place_contribution(face, dice, sides)
            tally = [
                current + added
                for current, added in zip(tally, contribution)
            ]
        if any(value != goal for value in tally):
            return False
    return True


def analyze_place_contributions(dice: int, sides: int) -> None:
    contributions = [
        place_contribution(face, dice, sides)
        for face in range(dice * sides)
    ]
    fixed_face_total = sides ** (dice - 1)
    if any(sum(item) != fixed_face_total for item in contributions):
        raise RuntimeError("place contribution row sum is inconsistent")
    if any(
        contributions[face][place]
        != contributions[-1 - face][dice - 1 - place]
        for face in range(dice * sides)
        for place in range(dice)
    ):
        raise RuntimeError("place contribution reversal symmetry failed")

    augmented = sympy.Matrix(
        [list(item) + [1] for item in contributions]
    )
    affine_nullity = len(augmented.nullspace())
    print("Place contribution structure:")
    print(
        f"  faces={dice * sides}, coordinates={dice}, "
        f"affine-rank={augmented.rank() - 1}, "
        f"affine-identities={affine_nullity}"
    )
    print(
        f"  universal face identity: sum_p contribution[face,p] "
        f"= {fixed_face_total}"
    )
    print(
        "  reversal identity: contribution[f,p] "
        "= contribution[FACE_COUNT-1-f,DICE-1-p]"
    )
    print(
        f"  a complete {dice}x{dice} place-tally matrix has fixed row and "
        f"column sums, leaving {(dice - 1) ** 2} independent deviations"
    )
    variables, equations = derive_place_moment_equations(dice, sides)
    print(
        "  compact per-die moments: "
        "S[j,q] = sum_column C(offset[column],j)*column^q"
    )
    print(
        f"  place fairness is exactly these {len(equations)} equations "
        f"over {len(variables)} incremental moments:"
    )
    for coefficients, rhs in equations:
        print(f"    {format_linear_equation(variables, coefficients, rhs)}")


def exact_row_join_theory(dice: int, sides: int) -> list[dict[str, int]]:
    """Describe exact additive meet-in-the-middle joins for built rows.

    Once rows before ``row`` are fixed, choosing a face for that row adds an
    independent amount to every ordering of the first ``row + 1`` dice and
    to the full-set place tallies.  Equality of all permutation buckets has
    factorial(order)-1 independent coordinates; place fairness has DICE-1.
    The first physical search column is canonical, leaving SIDES-1 variables.
    """

    variables = sides - 1
    left = variables // 2
    right = variables - left
    result: list[dict[str, int]] = []
    for row in range(1, dice - 1):
        order = row + 1
        radix = dice - row
        permutation_coordinates = math.factorial(order) - 1
        place_coordinates = dice - 1
        result.append(
            {
                "row": row,
                "order": order,
                "radix": radix,
                "variables": variables,
                "permutation_coordinates": permutation_coordinates,
                "place_coordinates": place_coordinates,
                "total_coordinates": (
                    permutation_coordinates + place_coordinates
                ),
                "left_states": radix**left,
                "right_states": radix**right,
                "total_states": radix**left + radix**right,
            }
        )
    return result


def analyze_exact_row_joins(dice: int, sides: int) -> None:
    print("Exact row-join opportunities:")
    print(
        "  coordinates = (row-prefix permutations minus one fixed-sum "
        "bucket) + (full places minus one fixed-sum coordinate)"
    )
    for item in exact_row_join_theory(dice, sides):
        print(
            f"  internal row {item['row']}: radix={item['radix']}, "
            f"variables={item['variables']}, exact-dimensions="
            f"{item['permutation_coordinates']} permutation + "
            f"{item['place_coordinates']} place = "
            f"{item['total_coordinates']}; balanced-MITM-states="
            f"{item['left_states']}+{item['right_states']}="
            f"{item['total_states']}"
        )


def conditioned_correlation_degree(
    feature: Feature, fixed_dice: int
) -> int:
    """Degree left after rows [0, fixed_dice) have been fixed.

    A within-column indicator is constant exactly when its block contains
    only already-built dice.  Every other block depends on the choice made
    for one of the remaining rows in that physical column.
    """

    return sum(
        any(owner >= fixed_dice for owner in block)
        for block in feature.blocks
    )


def conditionally_linear_full_directions(
    order: int, fixed_dice: int
) -> list[str]:
    """Full-order equations that become additive after fixing early rows."""

    permutations = list(itertools.permutations(range(order)))
    expansions = [collision_expansion(word) for word in permutations]
    result: list[str] = []
    for direction in simple_fairness_directions(permutations):
        expansion = combine_expansions(direction.coefficients, expansions)
        degree = max(
            (
                conditioned_correlation_degree(feature, fixed_dice)
                for feature in expansion
            ),
            default=0,
        )
        if degree <= 1:
            result.append(direction.name)
    return result


def analyze_conditioned_completion(dice: int) -> None:
    if dice < 4:
        return
    fixed_dice = dice - 2
    directions = conditionally_linear_full_directions(dice, fixed_dice)
    print(
        "Conditioned final-row identities:\n"
        f"  after rows A..{owner_name(fixed_dice - 1)} are fixed, "
        f"{len(directions)} of {math.factorial(dice) - 1} full-order "
        "fairness directions become exactly additive"
    )
    if len(directions) <= 24:
        print("  " + ", ".join(directions))


def encoding_to_columns(
    text: str, dice: int, sides: int
) -> tuple[list[int], list[Word]]:
    encoding = [ord(character.upper()) - ord("A") for character in text]
    if len(encoding) != dice * sides:
        raise ValueError(
            f"encoding length {len(encoding)} is not {dice * sides}"
        )
    expected = set(range(dice))
    columns = [
        tuple(encoding[start : start + dice])
        for start in range(0, len(encoding), dice)
    ]
    for index, column in enumerate(columns):
        if set(column) != expected or len(set(column)) != dice:
            raise ValueError(
                f"column {index} is not a permutation: {word_name(column)}"
            )
    return encoding, columns


def is_permutation_fair(
    counts: Mapping[Word, int], dice: int, sides: int
) -> bool:
    for state, count in counts.items():
        target_numerator = sides ** len(state)
        factorial = math.factorial(len(state))
        if target_numerator % factorial or count != target_numerator // factorial:
            return False
    return True


def is_all_subset_place_fair(
    counts: Mapping[Word, int], dice: int, sides: int
) -> bool:
    labels = range(dice)
    for size in range(2, dice + 1):
        numerator = sides ** size
        if numerator % size:
            return False
        goal = numerator // size
        for subset_tuple in itertools.combinations(labels, size):
            subset = frozenset(subset_tuple)
            orders = list(itertools.permutations(subset_tuple))
            for die in subset_tuple:
                for place in range(size):
                    tally = sum(
                        counts[order]
                        for order in orders
                        if relative_position(order, subset, die) == place
                    )
                    if tally != goal:
                        return False
    return True


def validate_collision_expansions(
    encoding: Sequence[int],
    columns: Sequence[Word],
    theories: Sequence[OrderTheory],
) -> None:
    dice = len(columns[0])
    counts = permutation_subsequence_counts(encoding, dice)
    positions = column_positions(columns)
    for theory in theories:
        for subset in itertools.combinations(range(dice), theory.order):
            relabel = dict(enumerate(subset))
            concrete_words = [
                tuple(relabel[owner] for owner in abstract)
                for abstract in theory.permutations
            ]
            concrete_expansions = [
                collision_expansion(word) for word in concrete_words
            ]
            canonical_expansions = (
                [
                    canonicalize_expansion(expansion, concrete_words)
                    for expansion in concrete_expansions
                ]
                if theory.within_column_canonicalized
                else []
            )
            features = {
                feature
                for expansion in concrete_expansions + canonical_expansions
                for feature in expansion
            }
            values = {
                feature: evaluate_feature(feature, positions)
                for feature in features
            }
            for word, expansion in zip(concrete_words, concrete_expansions):
                expanded = evaluate_expansion(expansion, values)
                if expanded != counts[word]:
                    raise RuntimeError(
                        f"collision expansion for {word_name(word)} gives "
                        f"{expanded}, DP gives {counts[word]}"
                    )
            for word, expansion in zip(concrete_words, canonical_expansions):
                expanded = evaluate_expansion(expansion, values)
                if expanded != counts[word]:
                    raise RuntimeError(
                        f"canonical collision expansion for {word_name(word)} "
                        f"gives {expanded}, DP gives {counts[word]}"
                    )


def read_recorded_solutions(
    path: Path,
    dice: int,
    sides: int,
    limit: int,
) -> dict[str, list[str]]:
    selected: dict[str, list[str]] = {
        "permutation-fair": [],
        "all-subset-place-fair": [],
    }
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, 1):
            match = CONFIGURATION_RE.match(line.strip())
            if match is None:
                continue
            if int(match.group("dice")) != dice or int(match.group("sides")) != sides:
                continue
            kind = match.group("kind")
            if limit == 0 or len(selected[kind]) < limit:
                selected[kind].append(match.group("encoding"))
            if limit and all(len(items) >= limit for items in selected.values()):
                break
    return selected


def validate_recorded_solutions(
    path: Path,
    dice: int,
    sides: int,
    theories: Sequence[OrderTheory],
    limit: int,
) -> None:
    selected = read_recorded_solutions(path, dice, sides, limit)
    total = sum(map(len, selected.values()))
    if total == 0:
        raise ValueError(
            f"{path} contains no recorded {dice}d{sides} configurations"
        )

    checked = defaultdict(int)
    moment_variables, moment_equations = derive_place_moment_equations(
        dice, sides
    )
    for kind, encodings in selected.items():
        for text in encodings:
            encoding, columns = encoding_to_columns(text, dice, sides)
            counts = permutation_subsequence_counts(encoding, dice)
            if kind == "permutation-fair":
                if not is_permutation_fair(counts, dice, sides):
                    raise RuntimeError(
                        "a recorded permutation-fair encoding failed DP verification"
                    )
            elif not is_all_subset_place_fair(counts, dice, sides):
                raise RuntimeError(
                    "a recorded all-subset-place-fair encoding failed verification"
                )
            if not place_moment_equations_hold(
                columns, moment_variables, moment_equations
            ):
                raise RuntimeError(
                    "a recorded place-fair encoding failed the moment equations"
                )
            validate_collision_expansions(encoding, columns, theories)
            checked[kind] += 1

    print(f"Validation against {path}:")
    print(
        "  " + ", ".join(
            f"{kind}={count}" for kind, count in sorted(checked.items())
        )
    )
    print("  all recorded properties and collision expansions verified")


def deterministic_columns(dice: int, sides: int, trial: int) -> list[Word]:
    base = tuple(range(dice))
    permutations = list(itertools.permutations(base))
    return [
        permutations[(column * (trial * 2 + 1) + trial) % len(permutations)]
        for column in range(sides)
    ]


def self_test(theories: Sequence[OrderTheory], dice: int, sides: int) -> None:
    test_sides = min(sides, 7)
    while test_sides > 1 and test_sides**dice % dice:
        test_sides -= 1
    if test_sides**dice % dice:
        test_sides = sides
    variables, equations = derive_place_moment_equations(dice, test_sides)
    for trial in range(5):
        columns = deterministic_columns(dice, test_sides, trial)
        encoding = [owner for column in columns for owner in column]
        validate_collision_expansions(encoding, columns, theories)
        direct = direct_full_place_fair(columns, dice, test_sides)
        moments = place_moment_equations_hold(columns, variables, equations)
        if direct != moments:
            raise RuntimeError(
                "place moment equations disagreed with direct place tallies"
            )
    print(
        f"Self-test: 5 deterministic unfair {dice}d{test_sides} "
        "configurations exactly matched the subsequence DP"
    )


def feature_to_json(feature: Feature) -> dict[str, object]:
    return {
        "blocks": [word_name(block) for block in feature.blocks],
        "gaps": list(feature.gaps),
        "correlation_degree": feature.correlation_degree,
        "text": format_feature(feature),
    }


def direction_to_json(
    theory: OrderTheory, direction: NamedDirection
) -> dict[str, object]:
    identity = direction_identity(theory, direction)
    return {
        "name": direction.name,
        "permutation_coefficients": {
            word_name(word): coefficient
            for word, coefficient in zip(
                theory.permutations, direction.coefficients
            )
            if coefficient
        },
        "cost": {
            "correlation_degree": identity_cost(identity)[0],
            "terms": identity_cost(identity)[1],
            "coefficient_l1": identity_cost(identity)[2],
        },
        "features": [
            {
                "coefficient": coefficient,
                **feature_to_json(feature),
            }
            for feature, coefficient in sorted(identity.items())
        ],
    }


def write_json_report(
    path: Path,
    dice: int,
    sides: int,
    theories: Sequence[OrderTheory],
) -> None:
    moment_variables, moment_equations = derive_place_moment_equations(
        dice, sides
    )
    report = {
        "dice": dice,
        "sides": sides,
        "place_moments": {
            "definition": "S[j,q] = sum_c binomial(offset[c],j)*c**q",
            "variables": [
                {"j": j, "q": q, "name": f"S[{j},{q}]"}
                for j, q in moment_variables
            ],
            "equations": [
                {
                    "coefficients": {
                        f"S[{j},{q}]": coefficient
                        for (j, q), coefficient in zip(
                            moment_variables, coefficients
                        )
                        if coefficient
                    },
                    "rhs": rhs,
                }
                for coefficients, rhs in moment_equations
            ],
        },
        "exact_row_joins": exact_row_join_theory(dice, sides),
        "orders": [],
    }
    for theory in theories:
        report["orders"].append(
            {
                "order": theory.order,
                "fair_target": sides ** theory.order
                // math.factorial(theory.order),
                "composition_count": 1 << (theory.order - 1),
                "within_column_canonicalized": (
                    theory.within_column_canonicalized
                ),
                "single_column_fair_rank": theory.single_column_fair_rank,
                "all_subset_spans_single_column": (
                    theory.all_subset_spans_single_column
                ),
                "full_place": [
                    direction_to_json(theory, item)
                    for item in theory.full_place_basis
                ],
                "all_subset_place": [
                    direction_to_json(theory, item)
                    for item in theory.all_subset_place_basis
                ],
                "permutation_only": [
                    direction_to_json(theory, item)
                    for item in theory.permutation_only_basis
                ],
            }
        )
    path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    arguments = parse_arguments()
    max_order = arguments.max_order or arguments.dice
    if not 2 <= max_order <= arguments.dice:
        print("error: --max-order must be between 2 and DICE", file=sys.stderr)
        return 2
    if arguments.dice > 6:
        print("error: this initial tool supports at most 6 dice", file=sys.stderr)
        return 2

    try:
        theories = [
            derive_order_theory(order) for order in range(2, max_order + 1)
        ]
        print(
            f"Column theory for {arguments.dice}d{arguments.sides} "
            f"through order {max_order} (SymPy {sympy.__version__})"
        )
        analyze_place_contributions(arguments.dice, arguments.sides)
        analyze_exact_row_joins(arguments.dice, arguments.sides)
        analyze_conditioned_completion(arguments.dice)

        for theory in theories:
            target_numerator = arguments.sides ** theory.order
            factorial = math.factorial(theory.order)
            target = (
                str(target_numerator // factorial)
                if target_numerator % factorial == 0
                else f"{target_numerator}/{factorial} (nonintegral)"
            )
            max_degree = max(
                feature.correlation_degree
                for expansion in theory.expansions
                for feature in expansion
            )
            print(
                f"\nOrder {theory.order}: permutations={factorial}, "
                f"fair-target={target}, compositions={1 << (theory.order - 1)}, "
                f"maximum-correlation-degree={max_degree}, "
                "within-column-reduction="
                f"{'exact' if theory.within_column_canonicalized else 'raw'}"
            )
            print(
                f"  full-place rank={len(theory.full_place_basis)}, "
                f"all-subset-place rank={len(theory.all_subset_place_basis)}, "
                f"permutation-only rank={len(theory.permutation_only_basis)}, "
                f"total={factorial - 1}"
            )
            if theory.single_column_fair_rank is not None:
                print(
                    "  fairness directions with no inter-column "
                    f"correlations={theory.single_column_fair_rank}; "
                    "all-subset-place spans them="
                    f"{str(theory.all_subset_spans_single_column).lower()}"
                )
            print_collision_example(theory)
            print_direction_family(
                "full-place identities",
                theory,
                theory.full_place_basis,
                arguments.show_identities,
                arguments.print_term_limit,
            )
            print_direction_family(
                "all-subset-place basis",
                theory,
                theory.all_subset_place_basis,
                arguments.show_identities,
                arguments.print_term_limit,
            )
            print_direction_family(
                "additional permutation-only basis",
                theory,
                theory.permutation_only_basis,
                arguments.show_identities,
                arguments.print_term_limit,
            )

        if arguments.self_test:
            self_test(theories, arguments.dice, arguments.sides)
        if arguments.solutions_file is not None:
            validate_recorded_solutions(
                arguments.solutions_file,
                arguments.dice,
                arguments.sides,
                theories,
                arguments.validate_limit,
            )
        if arguments.json is not None:
            write_json_report(
                arguments.json,
                arguments.dice,
                arguments.sides,
                theories,
            )
            print(f"Wrote machine-readable identities to {arguments.json}")
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
