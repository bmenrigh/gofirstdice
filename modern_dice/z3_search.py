#!/usr/bin/env python3
"""Search for Go First Dice and mine impossible prefixes with Z3.

The ordinary permutation-counting dynamic program becomes an exact network
of integer constraints.  For a permutation q ending in owner x,

    count[i + 1, q] = count[i, q]
                      + if owner[i] == x then count[i, q[:-1]] else 0.

Every final counter is constrained to its fair target.  Fixed face owners are
passed as named solver assumptions, so an unsatisfiable query can return a
smaller subset of assignments that already proves the prefix impossible.
Those cores are candidate pruning "nogoods" for the native search.
"""

from __future__ import annotations

import argparse
import itertools
import math
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

try:
    import z3
except ImportError as error:  # pragma: no cover - only used on missing setup
    print(
        "error: z3_search.py requires the Python Z3 bindings "
        "(install z3-solver or your distribution's Python Z3 package)",
        file=sys.stderr,
    )
    raise SystemExit(2) from error


@dataclass(frozen=True, order=True)
class Assignment:
    position: int
    owner: int

    @property
    def name(self) -> str:
        return f"fix_{self.position}_{self.owner}"


@dataclass
class MiningTotals:
    checks: int = 0
    satisfiable: int = 0
    unsatisfiable: int = 0
    unknown: int = 0
    cores: int = 0
    minimization_checks: int = 0


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
            "Encode the permutation-counting DP as exact Z3 constraints. "
            "Fixed assignments are assumptions, allowing impossible prefixes "
            "to produce unsatisfiable pruning cores."
        )
    )
    parser.add_argument("--dice", "-d", type=positive_int, default=4)
    parser.add_argument("--sides", "-s", type=positive_int, default=12)
    parser.add_argument(
        "--prefix",
        default="",
        help="fix a left prefix using letters A, B, ... or single digits",
    )
    parser.add_argument(
        "--suffix",
        default="",
        help="fix a right suffix in its normal left-to-right order",
    )
    parser.add_argument(
        "--fix",
        action="append",
        default=[],
        metavar="POSITION=OWNER",
        help="fix an arbitrary zero-based position; may be repeated",
    )
    parser.add_argument(
        "--no-symmetry",
        action="store_true",
        help="do not impose canonical first-occurrence order A, B, ...",
    )
    parser.add_argument(
        "--minimize-core",
        action="store_true",
        help="deletion-minimize each unsatisfiable assignment core",
    )
    parser.add_argument(
        "--mine-depth",
        type=positive_int,
        metavar="N",
        help="walk canonical prefixes through depth N and report dead cores",
    )
    parser.add_argument(
        "--max-checks",
        type=positive_int,
        default=1000,
        help="maximum solver checks in mining mode (default: 1000)",
    )
    parser.add_argument(
        "--max-cores",
        type=positive_int,
        default=25,
        help="stop mining after this many distinct cores (default: 25)",
    )
    parser.add_argument(
        "--timeout",
        type=nonnegative_int,
        default=30,
        metavar="SECONDS",
        help="timeout for each solver check; zero disables it (default: 30)",
    )
    parser.add_argument(
        "--arithmetic",
        choices=("bitvec", "int"),
        default="int",
        help="DP counter representation (default: int)",
    )
    parser.add_argument(
        "--emit-smt2",
        type=Path,
        help="write the constructed constraints and initial query as SMT-LIB",
    )
    return parser.parse_args()


def permutation_states(dice: int) -> list[tuple[int, ...]]:
    return [
        state
        for length in range(1, dice + 1)
        for state in itertools.permutations(range(dice), length)
    ]


def fair_targets(dice: int, sides: int) -> list[int]:
    targets = [1]
    for length in range(1, dice + 1):
        numerator = sides**length
        denominator = math.factorial(length)
        if numerator % denominator != 0:
            raise ValueError(
                f"{dice}d{sides} cannot be permutation-fair: "
                f"{sides}^{length} is not divisible by {length}!"
            )
        targets.append(numerator // denominator)
    return targets


def owner_name(owner: int) -> str:
    return chr(ord("A") + owner)


def parse_owner(text: str, dice: int) -> int:
    if len(text) == 1 and text.isalpha():
        owner = ord(text.upper()) - ord("A")
    else:
        owner = int(text)
    if not 0 <= owner < dice:
        raise ValueError(
            f"owner {text!r} is outside A-{owner_name(dice - 1)}"
        )
    return owner


def parse_owner_word(text: str, dice: int) -> list[int]:
    compact = "".join(character for character in text if character not in " ,:;")
    return [parse_owner(character, dice) for character in compact]


def collect_assignments(arguments: argparse.Namespace) -> list[Assignment]:
    face_count = arguments.dice * arguments.sides
    by_position: dict[int, int] = {}

    def add(position: int, owner: int) -> None:
        if not 0 <= position < face_count:
            raise ValueError(
                f"position {position} is outside 0 through {face_count - 1}"
            )
        previous = by_position.get(position)
        if previous is not None and previous != owner:
            raise ValueError(
                f"position {position} is assigned both "
                f"{owner_name(previous)} and {owner_name(owner)}"
            )
        by_position[position] = owner

    prefix = parse_owner_word(arguments.prefix, arguments.dice)
    suffix = parse_owner_word(arguments.suffix, arguments.dice)
    if len(prefix) > face_count or len(suffix) > face_count:
        raise ValueError("prefix or suffix is longer than the configuration")
    for position, owner in enumerate(prefix):
        add(position, owner)
    suffix_start = face_count - len(suffix)
    for offset, owner in enumerate(suffix):
        add(suffix_start + offset, owner)
    for specification in arguments.fix:
        if "=" not in specification:
            raise ValueError(
                f"invalid --fix {specification!r}; expected POSITION=OWNER"
            )
        position_text, owner_text = specification.split("=", 1)
        add(int(position_text), parse_owner(owner_text, arguments.dice))
    return [
        Assignment(position, owner)
        for position, owner in sorted(by_position.items())
    ]


class DiceModel:
    """One persistent Z3 model shared by solving and mining queries."""

    def __init__(
        self,
        dice: int,
        sides: int,
        canonical_labels: bool,
        timeout: int,
        arithmetic: str,
    ) -> None:
        self.dice = dice
        self.sides = sides
        self.face_count = dice * sides
        self.states = permutation_states(dice)
        self.state_index = {
            state: index for index, state in enumerate(self.states)
        }
        self.targets = fair_targets(dice, sides)
        self.arithmetic = arithmetic
        # A length-k counter can never exceed SIDES**k.  That inclusive
        # maximum determines an overflow-safe bit-vector width.
        self.width = [0] + [
            (sides**length).bit_length() for length in range(1, dice + 1)
        ]
        self.solver = z3.Solver()
        self.solver.set(unsat_core=True)
        if timeout:
            self.solver.set(timeout=timeout * 1000)
        self.choice = [
            [
                z3.Bool(f"owner_{position}_{owner}")
                for owner in range(dice)
            ]
            for position in range(self.face_count)
        ]
        self.count = [
            [
                (
                    z3.BitVec(
                        f"count_{position}_{index}",
                        self.width[len(state)],
                    )
                    if arithmetic == "bitvec"
                    else z3.Int(f"count_{position}_{index}")
                )
                for index, state in enumerate(self.states)
            ]
            for position in range(self.face_count + 1)
        ]
        self._literal_by_assignment: dict[Assignment, z3.BoolRef] = {}
        self._assignment_by_name: dict[str, Assignment] = {}
        self._build(canonical_labels)

    def _build(self, canonical_labels: bool) -> None:
        for choices in self.choice:
            self.solver.add(
                z3.PbEq([(choice, 1) for choice in choices], 1)
            )

        # Redundant with the singleton final counters, but useful propagation.
        for owner in range(self.dice):
            self.solver.add(
                z3.PbEq(
                    [
                        (self.choice[position][owner], 1)
                        for position in range(self.face_count)
                    ],
                    self.sides,
                )
            )

        if canonical_labels:
            self.solver.add(self.choice[0][0])
            for position in range(self.face_count):
                for owner in range(1, self.dice):
                    previously_seen = [
                        self.choice[earlier][owner - 1]
                        for earlier in range(position)
                    ]
                    self.solver.add(
                        z3.Implies(
                            self.choice[position][owner],
                            z3.Or(previously_seen)
                            if previously_seen
                            else z3.BoolVal(False),
                        )
                    )

        for position in range(self.face_count + 1):
            for index, state in enumerate(self.states):
                counter = self.count[position][index]
                if self.arithmetic == "bitvec":
                    self.solver.add(
                        z3.ULE(
                            counter,
                            z3.BitVecVal(
                                self.targets[len(state)],
                                self.width[len(state)],
                            ),
                        )
                    )
                else:
                    self.solver.add(
                        counter >= 0,
                        counter <= self.targets[len(state)],
                    )

        for counter in self.count[0]:
            self.solver.add(counter == 0)

        for position in range(self.face_count):
            previous = self.count[position]
            following = self.count[position + 1]
            for index, state in enumerate(self.states):
                if self.arithmetic == "bitvec":
                    width = self.width[len(state)]
                    if len(state) == 1:
                        source = z3.BitVecVal(1, width)
                    else:
                        source = previous[self.state_index[state[:-1]]]
                        source = z3.ZeroExt(width - source.size(), source)
                    zero = z3.BitVecVal(0, width)
                else:
                    source = (
                        z3.IntVal(1)
                        if len(state) == 1
                        else previous[self.state_index[state[:-1]]]
                    )
                    zero = z3.IntVal(0)
                self.solver.add(
                    following[index]
                    == previous[index]
                    + z3.If(
                        self.choice[position][state[-1]], source, zero
                    )
                )

        final = self.count[self.face_count]
        for index, state in enumerate(self.states):
            target = self.targets[len(state)]
            if self.arithmetic == "bitvec":
                target = z3.BitVecVal(target, self.width[len(state)])
            self.solver.add(final[index] == target)

    def literal(self, assignment: Assignment) -> z3.BoolRef:
        literal = self._literal_by_assignment.get(assignment)
        if literal is None:
            literal = z3.Bool(assignment.name)
            self.solver.add(
                z3.Implies(
                    literal,
                    self.choice[assignment.position][assignment.owner],
                )
            )
            self._literal_by_assignment[assignment] = literal
            self._assignment_by_name[literal.decl().name()] = assignment
        return literal

    def check(self, assignments: list[Assignment]) -> z3.CheckSatResult:
        return self.solver.check(
            *[self.literal(item) for item in assignments]
        )

    def core(self) -> list[Assignment]:
        core = []
        for literal in self.solver.unsat_core():
            assignment = self._assignment_by_name.get(
                literal.decl().name()
            )
            if assignment is not None:
                core.append(assignment)
        return sorted(core)

    def encoding(self) -> str:
        model = self.solver.model()
        encoding = []
        for choices in self.choice:
            selected = [
                owner
                for owner, choice in enumerate(choices)
                if z3.is_true(
                    model.eval(choice, model_completion=True)
                )
            ]
            if len(selected) != 1:
                raise RuntimeError(
                    "model does not select exactly one face owner"
                )
            encoding.append(owner_name(selected[0]))
        return "".join(encoding)

    def emit_smt2(
        self, path: Path, assignments: list[Assignment]
    ) -> None:
        literals = [self.literal(item) for item in assignments]
        query = self.solver.sexpr()
        if literals:
            query += "\n(check-sat-assuming (" + " ".join(
                literal.decl().name() for literal in literals
            ) + "))\n(get-unsat-core)\n"
        else:
            query += "\n(check-sat)\n(get-model)\n"
        path.write_text(query, encoding="utf-8")


def minimize_core(
    model: DiceModel, core: list[Assignment]
) -> tuple[list[Assignment], int, int]:
    """Return a subset-minimal core, number of checks, and unknown checks."""
    minimized = list(core)
    checks = 0
    unknown = 0
    index = 0
    while index < len(minimized):
        candidate = minimized[:index] + minimized[index + 1 :]
        result = model.check(candidate)
        checks += 1
        if result == z3.unsat:
            minimized = candidate
        else:
            if result == z3.unknown:
                unknown += 1
            index += 1
    # Leave the solver in the state corresponding to the reported core.
    if model.check(minimized) != z3.unsat:
        raise RuntimeError("core minimization lost unsatisfiability")
    return minimized, checks + 1, unknown


def verify_encoding(
    encoding: str, dice: int, targets: list[int]
) -> bool:
    states = permutation_states(dice)
    ways = {state: 0 for state in states}
    for letter in encoding:
        owner = ord(letter) - ord("A")
        previous = dict(ways)
        for state in states:
            if state[-1] == owner:
                source = 1 if len(state) == 1 else previous[state[:-1]]
                ways[state] = previous[state] + source
    return all(ways[state] == targets[len(state)] for state in states)


def format_core(core: list[Assignment], original_count: int) -> str:
    lines = [f"unsat-core={len(core)}/{original_count}"]
    for assignment in core:
        lines.append(
            f"  position {assignment.position} = {owner_name(assignment.owner)}"
        )
    if core:
        terms = " and ".join(
            f"position[{item.position}]={owner_name(item.owner)}"
            for item in core
        )
        lines.append(f"nogood: not ({terms})")
    return "\n".join(lines)


def canonical_extensions(
    prefix: list[int], dice: int, sides: int, canonical_labels: bool
) -> Iterator[int]:
    counts = [prefix.count(owner) for owner in range(dice)]
    if canonical_labels:
        used = max(prefix, default=-1) + 1
        limit = min(used + 1, dice)
    else:
        limit = dice
    for owner in range(limit):
        if counts[owner] < sides:
            yield owner


def mine_prefixes(
    model: DiceModel,
    initial_prefix: list[int],
    depth: int,
    canonical_labels: bool,
    minimize: bool,
    max_checks: int,
    max_cores: int,
) -> MiningTotals:
    totals = MiningTotals()
    seen_cores: set[frozenset[Assignment]] = set()
    started = time.monotonic()

    def visit(prefix: list[int]) -> None:
        if totals.checks >= max_checks or totals.cores >= max_cores:
            return
        assignments = [
            Assignment(position, owner) for position, owner in enumerate(prefix)
        ]
        result = model.check(assignments)
        totals.checks += 1
        if result == z3.unknown:
            totals.unknown += 1
            return
        if result == z3.unsat:
            totals.unsatisfiable += 1
            core = model.core()
            if minimize:
                core, checks, unknown = minimize_core(model, core)
                totals.minimization_checks += checks
                totals.unknown += unknown
            key = frozenset(core)
            if key not in seen_cores:
                seen_cores.add(key)
                totals.cores += 1
                print(
                    f"\nmined-core #{totals.cores} "
                    f"at-prefix={''.join(owner_name(x) for x in prefix)}"
                )
                print(format_core(core, len(assignments)))
            return

        totals.satisfiable += 1
        if len(prefix) == depth:
            return
        for owner in canonical_extensions(
            prefix, model.dice, model.sides, canonical_labels
        ):
            visit(prefix + [owner])
            if totals.checks >= max_checks or totals.cores >= max_cores:
                break

    visit(initial_prefix)
    elapsed = time.monotonic() - started
    print(
        f"\nMining summary: {elapsed:.3f}s, checks={totals.checks}, "
        f"sat={totals.satisfiable}, unsat={totals.unsatisfiable}, "
        f"unknown={totals.unknown}, distinct-cores={totals.cores}, "
        f"core-minimization-checks={totals.minimization_checks}"
    )
    return totals


def validate_mining_arguments(
    arguments: argparse.Namespace, assignments: list[Assignment]
) -> list[int]:
    if arguments.suffix or arguments.fix:
        raise ValueError("--mine-depth currently supports --prefix only")
    prefix = parse_owner_word(arguments.prefix, arguments.dice)
    if arguments.mine_depth < len(prefix):
        raise ValueError("--mine-depth is shorter than --prefix")
    expected = [Assignment(position, owner) for position, owner in enumerate(prefix)]
    if assignments != expected:
        raise ValueError("mining assignments must form a contiguous left prefix")
    if not arguments.no_symmetry:
        used = 0
        for owner in prefix:
            if owner > used:
                raise ValueError("--prefix violates canonical label introduction")
            if owner == used and used < arguments.dice:
                used += 1
    return prefix


def main() -> int:
    arguments = parse_arguments()
    try:
        assignments = collect_assignments(arguments)
        started = time.monotonic()
        model = DiceModel(
            arguments.dice,
            arguments.sides,
            not arguments.no_symmetry,
            arguments.timeout,
            arguments.arithmetic,
        )
        build_elapsed = time.monotonic() - started
        if arguments.emit_smt2 is not None:
            model.emit_smt2(arguments.emit_smt2, assignments)
    except (OSError, RuntimeError, ValueError, z3.Z3Exception) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    print(
        f"Encoded {arguments.dice}d{arguments.sides}: "
        f"faces={model.face_count}, states={len(model.states)}, "
        f"DP-transitions={model.face_count * len(model.states)}, "
        f"arithmetic={arguments.arithmetic}, fixed={len(assignments)}, "
        f"build-time={build_elapsed:.3f}s"
    )

    try:
        if arguments.mine_depth is not None:
            prefix = validate_mining_arguments(arguments, assignments)
            mine_prefixes(
                model,
                prefix,
                arguments.mine_depth,
                not arguments.no_symmetry,
                arguments.minimize_core,
                arguments.max_checks,
                arguments.max_cores,
            )
            return 0

        started = time.monotonic()
        result = model.check(assignments)
        elapsed = time.monotonic() - started
        print(f"result={result} check-time={elapsed:.3f}s")
        if result == z3.sat:
            encoding = model.encoding()
            if not verify_encoding(encoding, model.dice, model.targets):
                raise RuntimeError(
                    "the returned encoding failed independent DP verification"
                )
            print(f"encoding={encoding}")
            print("verified=permutation-fair")
            return 0
        if result == z3.unknown:
            print(f"reason-unknown={model.solver.reason_unknown()}")
            return 3
        if not assignments:
            print("The complete configuration is impossible under these constraints.")
            return 1

        core = model.core()
        if not core:
            raise RuntimeError("Z3 returned an empty unsat core")
        if arguments.minimize_core:
            started = time.monotonic()
            core, checks, unknown = minimize_core(model, core)
            print(
                f"core-minimization-checks={checks}, unknown={unknown}, "
                f"time={time.monotonic() - started:.3f}s"
            )
        print(format_core(core, len(assignments)))
        return 1
    except (RuntimeError, z3.Z3Exception) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
