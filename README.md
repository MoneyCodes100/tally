# Tally

Tally is a streaming-analytics library. The sketches are C++. Python is the API and the command-line tool.

A sketch reads a stream once, keeps a small summary, and answers a question that would otherwise require storing every event:

| Question | Sketch | Memory |
| --- | --- | --- |
| How many distinct keys? | HyperLogLog | `2^precision` bytes |
| Have we seen this key? | Bloom filter | sized from capacity and error rate |
| About how many times did this key appear? | Count-Min Sketch | `width * depth` counters |
| Which keys are the heavy hitters? | Space-Saving | `k` counters |
| What do the quantiles look like? | Reservoir sample | `capacity` numbers |

Count-Min and Space-Saving report upper bounds. A Bloom filter remembers every key it has stored, and sometimes claims it has stored a new one. HyperLogLog and the reservoir answer with a controllable error.

## Where to look

| Path | What it is |
| --- | --- |
| `include/tally/` | The algorithms. No Python types in these headers. |
| `src/tally/_tally.cpp` | The pybind11 boundary. |
| `src/tally/cli.py` | The `tally` command. |
| `tests/cpp/test_tally.cpp` | Invariants: no false negatives, merge equality, count bounds. |
| `tests/python/test_tally.py` | The same guarantees through the binding, plus the CLI. |
| `examples/demo.py` | One synthetic stream, exact answer next to each sketch. |
| `examples/cardinality.cpp` | The C++ API in a complete program. |

The hot path is branches, bit operations, and tight arrays, so it lives in C++ and the memory layout is visible. Python is the surface you script and ship: build a sketch, merge another worker's snapshot, or point the CLI at a log file. A C++ service can include `tally/tally.hpp` and skip the interpreter.

## Build

C++20, CMake 3.15 or newer, and Python 3.10 or newer. The Python package also needs a compiler.

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"
pytest
python examples/demo.py
```

C++ tests do not need the Python package:

```bash
cmake -S . -B build -DTALLY_BUILD_TESTS=ON -DTALLY_BUILD_PYTHON=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/tally_example tests/data/words.txt
```

`make test` runs both. Snapshots and MurmurHash3 x64 are little-endian.

## Python

```python
from tally import HyperLogLog, SpaceSaving

unique = HyperLogLog(precision=14)
heavy = SpaceSaving(k=10)
for line in open("events.log", encoding="utf-8"):
    key = line.rstrip("\n")
    unique.add(key)
    heavy.add(key)

print(round(unique.estimate()))
for item in heavy.top(5):
    # The true count is in [count - error, count].
    print(item.key, item.count, item.error)
```

`to_bytes()` / `from_bytes()` is the snapshot format, and the objects pickle. HyperLogLog, Count-Min, and Bloom filters merge. Two sketches of a split stream merge to the sketch of the whole stream.

```python
raw = unique.to_bytes()
copy = HyperLogLog.from_bytes(raw)
copy.merge(other)
```

Count-Min is the mergeable frequency sketch. `estimate(key)` is never below the true count. `from_error(epsilon, delta)` sizes the table so the overestimate is at most `epsilon` times the total count with probability at least `1 - delta`.

```python
from tally import CountMinSketch

counts = CountMinSketch.from_error(epsilon=0.001, delta=0.01)
counts.add("timeout", 3)
counts.estimate("timeout")
```

Sketches are not safe to share across threads without an external lock.

## Command line

```bash
tally card events.log --exact
tally freq events.log --top 10
tally quantiles values.log --p 0.5,0.95,0.99
tally bloom members.log --query queries.log
```

`card --exact` also keeps a Python set, so you can read the error on a file that fits in memory. Omit the path and the command reads stdin. On the small file in this repo:

```bash
tally card tests/data/words.txt --exact
```

```text
lines: 6
estimate: 3.00
sketch_bytes: 16384
exact: 3
relative_error: 0.01%
```

`python examples/demo.py` runs every sketch on one generated stream and prints the exact answer beside it:

```text
tally demo  events=200000  distinct=489

cardinality
  hyperloglog     489.2
  exact           489
  relative_error  0.05%
  sketch_bytes    16384
  merge_matches   true
  50000 distinct  50456.4  (0.91%)

key             exact    space    error    lower countmin
user-1         111407   111407        0   111407   111407
user-2          33349    33349        0    33349    33349
user-3          15884    15884        0    15884    15884
user-4           9042     9042        0     9042     9042
user-5           6027     6027        0     6027     6027

quantiles  reservoir=20000 of 200000
  p50   sketch=  499.67  exact=  499.25
  p95   sketch=  950.31  exact=  949.74
  p99   sketch=  989.86  exact=  989.89
```

The 50,000-key figure is the interesting HyperLogLog range: precision 14 has a standard error near 0.8%, and this run landed at 0.91% while using 16 KB. Merging the two halves of the stream reproduced the sketch of the whole stream byte for byte. The heavy hitters are exact because each of those keys stayed in the Space-Saving summary.

## C++

```cpp
#include "tally/hyperloglog.hpp"

tally::HyperLogLog unique(14);
unique.add(line);
double distinct = unique.estimate();
```

`examples/cardinality.cpp` is a complete program over the same header.

## Guarantees

These are the properties the tests lock in.

- **Bloom filter.** Every inserted key is reported present. The bit count is `ceil(-n ln p / (ln 2)^2)` and the hash count is about `(m/n) ln 2`, using Kirsch-Mitzenmacher double hashing. Merging is a bitwise OR of two filters with the same parameters.
- **HyperLogLog.** Standard raw estimate plus the small-range linear-counting correction from Flajolet et al. Precision 14 is 16 KB and a standard error near `1.04 / sqrt(2^p)`. The hash is 64-bit, so the 32-bit large-range correction is omitted. Merge takes the max of each register. Splitting a stream, sketching the halves, and merging them reproduces the sketch of the whole stream.
- **Count-Min.** `estimate(key) >= true count`, always. Merge adds the counters, saturating at `2^64 - 1`.
- **Space-Saving.** A monitored key's true count is in `[count - error, count]`. An evicted key's true count is at most the smallest counter. On a tie, the lexicographically smallest key is the one replaced, so the summary does not depend on hash-table iteration order. The counters sum to the total weight of the stream.
- **Reservoir.** Algorithm R. The first `capacity` values are kept in order. `quantile` linearly interpolates the sorted sample. The generator is SplitMix64, and a snapshot restores the generator state so the sample can resume.

MurmurHash3 x64 128 (Austin Appleby, public domain) is the shared hash. The C++ tests check it against known vectors.

## References

- Burton H. Bloom, "Space/Time Trade-offs in Hash Coding with Allowable Errors", 1970.
- Adam Kirsch and Michael Mitzenmacher, "Less Hashing, Same Performance: Building a Better Bloom Filter", 2006.
- Philippe Flajolet, Éric Fusy, Olivier Gandouet, Frédéric Meunier, "HyperLogLog: the analysis of a near-optimal cardinality estimation algorithm", 2007.
- Graham Cormode and S. Muthukrishnan, "An Improved Data Stream Summary: The Count-Min Sketch and its Applications", 2005.
- Ahmed Metwally, Divyakant Agrawal, Amr El Abbadi, "Efficient Computation of Frequent and Top-k Elements in Data Streams", 2005.
- Jeffrey S. Vitter, "Random Sampling with a Reservoir", 1985.

## License

MIT. See [LICENSE](LICENSE).
