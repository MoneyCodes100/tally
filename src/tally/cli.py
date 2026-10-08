"""Read a text file once and answer with a sketch."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Iterator

from tally import BloomFilter, HyperLogLog, Reservoir, SpaceSaving, __version__


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except FileNotFoundError as exc:
        name = exc.filename or "input"
        print(f"tally: {name}: no such file", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"tally: {exc}", file=sys.stderr)
        return 1
    except (ValueError, OverflowError) as exc:
        print(f"tally: {exc}", file=sys.stderr)
        return 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="tally",
        description="Bounded-memory summaries for a stream of lines.",
    )
    parser.add_argument("--version", action="version", version=f"tally {__version__}")
    commands = parser.add_subparsers(dest="command", required=True)

    card = commands.add_parser("card", help="Estimate how many distinct lines a file contains.")
    card.add_argument("path", nargs="?", help="One key per line. Omit to read stdin.")
    card.add_argument("--precision", type=precision, default=14, help="HyperLogLog precision, 4..16.")
    card.add_argument(
        "--exact",
        action="store_true",
        help="Also store every line in a set and print the true cardinality.",
    )
    card.set_defaults(func=cmd_card)

    freq = commands.add_parser("freq", help="List the heavy hitters with Space-Saving.")
    freq.add_argument("path", nargs="?", help="One key per line. Omit to read stdin.")
    freq.add_argument("--top", type=positive_int, default=10, help="How many keys to print.")
    freq.add_argument(
        "--counters",
        type=positive_int,
        default=100,
        help="How many counters to keep. Raise this when the tail matters.",
    )
    freq.set_defaults(func=cmd_freq)

    quant = commands.add_parser("quantiles", help="Estimate quantiles from a reservoir sample.")
    quant.add_argument("path", nargs="?", help="One number per line. Omit to read stdin.")
    quant.add_argument("--sample", type=positive_int, default=20_000, help="Reservoir size.")
    quant.add_argument("--seed", type=non_negative_int, default=0x5EED1234C0FFEE)
    quant.add_argument("--p", default="0.5,0.9,0.99", help="Comma-separated quantiles in [0, 1].")
    quant.set_defaults(func=cmd_quantiles)

    bloom = commands.add_parser("bloom", help="Build a Bloom filter and test query lines against it.")
    bloom.add_argument("members", help="Lines inserted into the filter.")
    bloom.add_argument("--query", help="Lines to test. Each is printed with yes or no.")
    bloom.add_argument("--capacity", type=positive_int, default=100_000)
    bloom.add_argument("--error-rate", type=unit_float, default=0.01)
    bloom.set_defaults(func=cmd_bloom)
    return parser


def cmd_card(args: argparse.Namespace) -> int:
    sketch = HyperLogLog(args.precision)
    exact: set[str] | None = set() if args.exact else None
    rows = 0
    for line in iter_lines(args.path):
        sketch.add(line)
        if exact is not None:
            exact.add(line)
        rows += 1
    print(f"lines: {rows}")
    print(f"estimate: {sketch.estimate():.2f}")
    print(f"sketch_bytes: {sketch.size_bytes}")
    if exact is not None:
        truth = len(exact)
        print(f"exact: {truth}")
        if truth == 0:
            print("relative_error: 0.00%")
        else:
            error = abs(sketch.estimate() - truth) / truth * 100
            print(f"relative_error: {error:.2f}%")
    return 0


def cmd_freq(args: argparse.Namespace) -> int:
    if args.top > args.counters:
        raise ValueError("--top cannot be larger than --counters")
    summary = SpaceSaving(args.counters)
    rows = 0
    for line in iter_lines(args.path):
        summary.add(line)
        rows += 1
    print(f"lines: {rows}")
    print("rank\tkey\tcount\terror\tlower")
    for rank, item in enumerate(summary.top(args.top), start=1):
        lower = item.count - item.error
        print(f"{rank}\t{item.key}\t{item.count}\t{item.error}\t{lower}")
    return 0


def cmd_quantiles(args: argparse.Namespace) -> int:
    levels = parse_levels(args.p)
    sample = Reservoir(args.sample, seed=args.seed)
    skipped = 0
    for line in iter_lines(args.path):
        if line == "":
            continue
        try:
            value = float(line)
        except ValueError:
            skipped += 1
            continue
        sample.add(value)
    if sample.seen == 0:
        raise ValueError("no numeric values")
    print(f"seen: {sample.seen}")
    print(f"sample: {sample.size}")
    print(f"sketch_bytes: {sample.size_bytes}")
    if skipped:
        print(f"skipped: {skipped}")
    for level in levels:
        print(f"{percentile_label(level)}: {sample.quantile(level):.6g}")
    return 0


def cmd_bloom(args: argparse.Namespace) -> int:
    sketch = BloomFilter(args.capacity, args.error_rate)
    members = 0
    for line in iter_lines(args.members):
        sketch.add(line)
        members += 1
    print(f"members: {members}")
    print(f"bits: {sketch.bit_count}")
    print(f"hashes: {sketch.hash_count}")
    print(f"sketch_bytes: {sketch.size_bytes}")
    if args.query:
        for line in iter_lines(args.query):
            answer = "yes" if line in sketch else "no"
            print(f"{line}\t{answer}")
    return 0


def iter_lines(path: str | None) -> Iterator[str]:
    if path is None:
        for line in sys.stdin:
            yield line.rstrip("\n\r")
        return
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            yield line.rstrip("\n\r")


def parse_levels(text: str) -> list[float]:
    levels: list[float] = []
    for piece in text.split(","):
        piece = piece.strip()
        if piece == "":
            continue
        try:
            level = float(piece)
        except ValueError as exc:
            raise ValueError(f"bad quantile {piece!r}") from exc
        if level < 0 or level > 1:
            raise ValueError(f"quantile {piece} is outside [0, 1]")
        levels.append(level)
    if not levels:
        raise ValueError("provide at least one quantile")
    return levels


def percentile_label(level: float) -> str:
    percent = round(level * 100, 6)
    if percent == int(percent):
        return f"p{int(percent)}"
    return f"p{percent:g}"


def positive_int(text: str) -> int:
    return bounded_int(text, minimum=1, label="expected a positive integer")


def non_negative_int(text: str) -> int:
    return bounded_int(text, minimum=0, label="expected a non-negative integer")


def bounded_int(text: str, minimum: int, label: str) -> int:
    try:
        value = int(text, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(label) from exc
    if value < minimum:
        raise argparse.ArgumentTypeError(label)
    return value


def precision(text: str) -> int:
    value = positive_int(text)
    if value < 4 or value > 16:
        raise argparse.ArgumentTypeError("precision must be between 4 and 16")
    return value


def unit_float(text: str) -> float:
    try:
        value = float(text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected a probability between 0 and 1") from exc
    if not 0 < value < 1:
        raise argparse.ArgumentTypeError("expected a probability between 0 and 1")
    return value


if __name__ == "__main__":
    raise SystemExit(main())
