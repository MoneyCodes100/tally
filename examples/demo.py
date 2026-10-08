"""Print a side-by-side check of every sketch on one synthetic stream.

Run it after `pip install -e .`:

    python examples/demo.py
"""

from __future__ import annotations

import math
import random
from collections import Counter

from tally import CountMinSketch, HyperLogLog, Reservoir, SpaceSaving


def synthetic(count: int, seed: int = 7) -> tuple[list[str], list[float]]:
    rng = random.Random(seed)
    keys: list[str] = []
    values: list[float] = []
    for _ in range(count):
        unit = max(rng.random(), 1e-12)
        rank = min(4000, max(1, int(unit**-0.85)))
        keys.append(f"user-{rank}")
        values.append(rng.random() * 1000.0)
    return keys, values


def exact_quantile(values: list[float], level: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = level * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def main() -> None:
    event_count = 200_000
    keys, values = synthetic(event_count)
    exact = Counter(keys)

    whole = HyperLogLog(14)
    left = HyperLogLog(14)
    right = HyperLogLog(14)
    counts = CountMinSketch.from_error(0.005, 0.001)
    heavy = SpaceSaving(50)
    sample = Reservoir(20_000, seed=7)

    for index, key in enumerate(keys):
        whole.add(key)
        (left if index % 2 == 0 else right).add(key)
        counts.add(key)
        heavy.add(key)
    for value in values:
        sample.add(value)
    left.merge(right)

    cardinality = len(exact)
    estimate = whole.estimate()
    print(f"tally demo  events={event_count}  distinct={cardinality}")
    print()
    print("cardinality")
    print(f"  hyperloglog     {estimate:.1f}")
    print(f"  exact           {cardinality}")
    print(f"  relative_error  {abs(estimate - cardinality) / cardinality * 100:.2f}%")
    print(f"  sketch_bytes    {whole.size_bytes}")
    print(f"  merge_matches   {str(left.to_bytes() == whole.to_bytes()).lower()}")
    scale = 50_000
    wide = HyperLogLog(14)
    wide.update(f"id-{i}" for i in range(scale))
    wide_estimate = wide.estimate()
    print(f"  {scale} distinct  {wide_estimate:.1f}  ({abs(wide_estimate - scale) / scale * 100:.2f}%)")
    print()
    print(f"{'key':<12} {'exact':>8} {'space':>8} {'error':>8} {'lower':>8} {'countmin':>8}")
    for item in heavy.top(5):
        print(
            f"{item.key:<12} {exact[item.key]:8d} {item.count:8d} "
            f"{item.error:8d} {item.count - item.error:8d} {counts.estimate(item.key):8d}"
        )
    print()
    print(f"quantiles  reservoir={sample.size} of {sample.seen}")
    for level in (0.5, 0.95, 0.99):
        label = f"p{int(level * 100)}"
        got = sample.quantile(level)
        truth = exact_quantile(values, level)
        print(f"  {label:<4}  sketch={got:8.2f}  exact={truth:8.2f}")


if __name__ == "__main__":
    main()
