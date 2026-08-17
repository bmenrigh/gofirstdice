#!/usr/bin/env python3
"""Expand smaller column-grouped solutions into larger search templates."""

from __future__ import annotations

import argparse
import hashlib
import itertools
import math
import random
import re
import sys
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path


ENCODING_RE = re.compile(r"\bencoding\s*=\s*([A-Za-z]+)\b")
RAW_ENCODING_RE = re.compile(r"[A-Za-z]+")


@dataclass(frozen=True)
class Solution:
    encoding: str
    dice: int
    sides: int
    columns: tuple[str, ...]
    mirrored: bool


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
            "Embed the leading columns of smaller column-grouped solutions "
            "into non-adjacent columns of larger templates. By default this "
            "uses a mirrored 4d12 source to seed an unmirrored 5d30 search: "
            "it embeds six leading source columns plus two randomly selected "
            "mirrored counterparts, uses the first fifteen target columns "
            "for the leading columns, and inserts the new die in the middle."
        )
    )
    parser.add_argument(
        "input",
        help=(
            "a raw encoding, or a file containing raw encodings or "
            "column_search 'encoding=...' lines; use '-' for standard input"
        ),
    )
    parser.add_argument("output_directory", type=Path)
    parser.add_argument(
        "--target-sides",
        type=positive_int,
        default=30,
        help="sides on each target die (default: 30)",
    )
    parser.add_argument(
        "--source-columns",
        type=positive_int,
        help="leading source columns to embed (default: half the source)",
    )
    parser.add_argument(
        "--target-columns",
        type=positive_int,
        help="leading target columns available (default: half the target)",
    )
    parser.add_argument(
        "--insert-at",
        type=nonnegative_int,
        help=(
            "zero-based position of the new die and value offset "
            "(default: middle)"
        ),
    )
    parser.add_argument(
        "--blank-gap",
        type=nonnegative_int,
        default=1,
        help="minimum blank columns between embedded columns (default: 1)",
    )
    parser.add_argument(
        "--extra-mirror-columns",
        type=nonnegative_int,
        default=2,
        help=(
            "mirrored source counterparts to add at mirrored target "
            "positions (default: 2)"
        ),
    )
    parser.add_argument(
        "--seed",
        type=nonnegative_int,
        default=1,
        help="seed for selecting extra mirrored columns (default: 1)",
    )
    parser.add_argument(
        "--max-templates",
        type=positive_int,
        default=100_000,
        help="refuse to create more than this many files (default: 100000)",
    )
    return parser.parse_args()


def read_input(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    if RAW_ENCODING_RE.fullmatch(path):
        return path
    return Path(path).read_text(encoding="utf-8")


def extract_encodings(text: str) -> list[str]:
    encodings = [match.upper() for match in ENCODING_RE.findall(text)]
    if not encodings:
        for line_number, line in enumerate(text.splitlines(), start=1):
            candidate = line.split("#", 1)[0].strip()
            if not candidate:
                continue
            if not RAW_ENCODING_RE.fullmatch(candidate):
                raise ValueError(
                    f"line {line_number} is neither a raw encoding nor a "
                    "column_search result"
                )
            encodings.append(candidate.upper())
    if not encodings:
        raise ValueError("the input contains no solution encodings")
    if len(encodings) != len(set(encodings)):
        raise ValueError("the input contains a duplicate solution encoding")
    return encodings


def decode_solution(encoding: str) -> Solution:
    owners = sorted(set(encoding))
    if not owners or owners != [chr(ord("A") + i) for i in range(len(owners))]:
        raise ValueError(
            f"encoding {encoding!r} must use contiguous die names starting at A"
        )
    dice = len(owners)
    if dice < 2:
        raise ValueError(f"encoding {encoding!r} must describe at least two dice")
    if len(encoding) % dice != 0:
        raise ValueError(
            f"encoding length {len(encoding)} is not divisible by {dice} dice"
        )
    sides = len(encoding) // dice
    columns = tuple(
        encoding[start : start + dice] for start in range(0, len(encoding), dice)
    )
    expected_column = owners
    for column_number, column in enumerate(columns):
        if sorted(column) != expected_column:
            raise ValueError(
                f"source column {column_number} is {column!r}; each column must "
                f"contain {''.join(expected_column)!r} exactly once"
            )
    mirrored = sides % 2 == 0 and all(
        columns[sides - column - 1] == columns[column][::-1]
        for column in range(sides // 2)
    )
    return Solution(encoding, dice, sides, columns, mirrored)


def destination_sets(
    source_columns: int, target_columns: int, blank_gap: int
) -> Iterator[tuple[int, ...]]:
    compressed_columns = target_columns - blank_gap * (source_columns - 1)
    if compressed_columns < source_columns:
        raise ValueError(
            f"cannot place {source_columns} source columns among "
            f"{target_columns} target columns with {blank_gap} blank columns "
            "between them"
        )
    return (
        tuple(value + index * blank_gap for index, value in enumerate(choice))
        for choice in itertools.combinations(
            range(compressed_columns), source_columns
        )
    )


def select_extra_mirror_columns(
    solution: Solution,
    destinations: tuple[int, ...],
    count: int,
    seed: int,
) -> tuple[int, ...]:
    if count == 0:
        return ()
    material = (
        f"{seed}:{solution.encoding}:"
        + ",".join(str(column) for column in destinations)
    )
    derived_seed = int.from_bytes(
        hashlib.sha256(material.encode("ascii")).digest()
    )
    generator = random.Random(derived_seed)
    return tuple(sorted(generator.sample(range(len(destinations)), count)))


def render_template(
    solution: Solution,
    target_sides: int,
    insert_at: int,
    source_columns: int,
    destinations: tuple[int, ...],
    extra_mirror_columns: tuple[int, ...],
) -> str:
    target_dice = solution.dice + 1
    values = [["." for _ in range(target_sides)] for _ in range(target_dice)]
    placements = list(enumerate(destinations))

    mirrored_placements = tuple(
        (
            solution.sides - source_column - 1,
            target_sides - destinations[source_column] - 1,
        )
        for source_column in extra_mirror_columns
    )
    placements.extend(mirrored_placements)

    for source_column, target_column in placements:
        for source_offset, owner in enumerate(solution.columns[source_column]):
            source_die = ord(owner) - ord("A")
            target_die = source_die + (source_die >= insert_at)
            target_offset = source_offset + (source_offset >= insert_at)
            values[target_die][target_column] = str(target_offset)

    field_width = max(2, len(str(target_sides - 1)))
    lines = [
        "# Generated by template_from_solution.py",
        (
            f"# Source: {solution.dice}d{solution.sides} "
            f"{'mirrored' if solution.mirrored else 'non-mirrored'} "
            f"encoding={solution.encoding}"
        ),
        (
            f"# Leading source columns 0-{source_columns - 1} -> target columns "
            + ",".join(str(column) for column in destinations)
        ),
    ]
    if mirrored_placements:
        lines.append(
            "# Mirrored source columns "
            + ",".join(str(source) for source, _ in mirrored_placements)
            + " -> target columns "
            + ",".join(str(target) for _, target in mirrored_placements)
        )
    lines.extend(
        [
            f"# Inserted die: {chr(ord('A') + insert_at)} (offset {insert_at})",
            "  "
            + "".join(
                f" {column:>{field_width}}" for column in range(target_sides)
            ),
        ]
    )
    for die, row in enumerate(values):
        lines.append(
            f"{chr(ord('A') + die)}:"
            + "".join(f" {value:>{field_width}}" for value in row)
        )
    return "\n".join(lines) + "\n"


def main() -> int:
    args = parse_arguments()
    try:
        encodings = extract_encodings(read_input(args.input))
        solutions = [decode_solution(value) for value in encodings]
        shape = (solutions[0].dice, solutions[0].sides)
        if any((solution.dice, solution.sides) != shape for solution in solutions):
            raise ValueError("all input solutions must have the same dice and sides")

        source_dice, source_sides = shape
        target_dice = source_dice + 1
        insert_at = args.insert_at
        if insert_at is None:
            insert_at = target_dice // 2
        if insert_at >= target_dice:
            raise ValueError(
                f"--insert-at must be between 0 and {target_dice - 1}"
            )

        source_columns = args.source_columns
        if source_columns is None:
            if any(not solution.mirrored for solution in solutions):
                raise ValueError(
                    "using half of each source requires mirror symmetry; "
                    "specify --source-columns to intentionally use a prefix "
                    "of a non-mirrored solution"
                )
            if source_sides % 2 != 0:
                raise ValueError(
                    "an odd-sided source requires an explicit --source-columns"
                )
            source_columns = source_sides // 2
        if source_columns > source_sides:
            raise ValueError("--source-columns exceeds the source side count")
        if args.extra_mirror_columns > source_columns:
            raise ValueError(
                "--extra-mirror-columns exceeds --source-columns"
            )
        if args.extra_mirror_columns != 0:
            if any(not solution.mirrored for solution in solutions):
                raise ValueError(
                    "extra mirrored columns require mirror-symmetric sources"
                )
            if source_columns > source_sides // 2:
                raise ValueError(
                    "extra mirrored columns require source columns from the "
                    "first half"
                )

        target_columns = args.target_columns
        if target_columns is None:
            if args.target_sides % 2 != 0:
                raise ValueError(
                    "an odd-sided target requires an explicit --target-columns"
                )
            target_columns = args.target_sides // 2
        if target_columns > args.target_sides:
            raise ValueError("--target-columns exceeds the target side count")
        if args.extra_mirror_columns != 0:
            if args.target_sides % 2 != 0:
                raise ValueError(
                    "extra mirrored columns require an even-sided target"
                )
            if target_columns > args.target_sides // 2:
                raise ValueError(
                    "extra mirrored columns require target columns from the "
                    "first half"
                )

        compressed_columns = target_columns - args.blank_gap * (source_columns - 1)
        if compressed_columns < source_columns:
            # Produce the detailed placement error in one place.
            tuple(destination_sets(source_columns, target_columns, args.blank_gap))
        per_solution = math.comb(compressed_columns, source_columns)
        total = per_solution * len(solutions)
        if total > args.max_templates:
            raise ValueError(
                f"this request would create {total} templates; increase "
                "--max-templates to allow it"
            )

        destinations = tuple(
            destination_sets(source_columns, target_columns, args.blank_gap)
        )
        output_directory = args.output_directory
        if output_directory.exists() and not output_directory.is_dir():
            raise ValueError(f"output path {output_directory} is not a directory")
        output_directory.mkdir(parents=True, exist_ok=True)

        column_width = max(2, len(str(target_columns - 1)))
        planned: list[
            tuple[Path, Solution, tuple[int, ...], tuple[int, ...]]
        ] = []
        for solution in solutions:
            identity = hashlib.sha256(
                solution.encoding.encode("ascii")
            ).hexdigest()[:16]
            for columns in destinations:
                extra_columns = select_extra_mirror_columns(
                    solution,
                    columns,
                    args.extra_mirror_columns,
                    args.seed,
                )
                mirrored_targets = tuple(
                    args.target_sides - columns[column] - 1
                    for column in extra_columns
                )
                all_target_columns = tuple(sorted(columns + mirrored_targets))
                column_tag = "-".join(
                    f"{column:0{column_width}d}"
                    for column in all_target_columns
                )
                filename = (
                    f"{target_dice}d{args.target_sides}_{identity}_"
                    f"cols_{column_tag}.template"
                )
                planned.append(
                    (output_directory / filename, solution, columns, extra_columns)
                )

        collisions = [path for path, _, _, _ in planned if path.exists()]
        if collisions:
            examples = ", ".join(str(path) for path in collisions[:3])
            suffix = " ..." if len(collisions) > 3 else ""
            raise ValueError(
                f"refusing to overwrite {len(collisions)} existing templates: "
                f"{examples}{suffix}"
            )

        for path, solution, columns, extra_columns in planned:
            with path.open("x", encoding="utf-8") as output:
                output.write(
                    render_template(
                        solution,
                        args.target_sides,
                        insert_at,
                        source_columns,
                        columns,
                        extra_columns,
                    )
                )
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    mirror_count = sum(solution.mirrored for solution in solutions)
    print(
        f"Wrote {total} {target_dice}d{args.target_sides} templates "
        f"({per_solution} per source) to {args.output_directory}; "
        f"sources={len(solutions)}, mirrored={mirror_count}, "
        f"columns-per-template={source_columns + args.extra_mirror_columns}, "
        f"blank-gap={args.blank_gap}, seed={args.seed}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
