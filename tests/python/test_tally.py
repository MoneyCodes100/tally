import copy
import io
import pickle
from collections import Counter

import pytest

from tally import BloomFilter, CountMinSketch, HyperLogLog, Reservoir, SpaceSaving
from tally.cli import main


def test_bloom_has_no_false_negatives_and_round_trips():
    sketch = BloomFilter(1000, 0.01)
    keys = [f"user-{i}" for i in range(1000)]
    sketch.update(keys)
    assert all(key in sketch for key in keys)
    absent = [f"other-{i}" for i in range(1000)]
    false_positives = sum(key in sketch for key in absent)
    assert false_positives < 50

    restored = BloomFilter.from_bytes(sketch.to_bytes())
    assert restored == sketch
    assert pickle.loads(pickle.dumps(sketch)) == sketch
    assert copy.deepcopy(sketch) == sketch

    with pytest.raises(ValueError):
        BloomFilter(10, 0)


def test_bloom_merge_matches_one_filter():
    left = BloomFilter(400, 0.02)
    right = BloomFilter(400, 0.02)
    both = BloomFilter(400, 0.02)
    for i in range(100):
        key = f"k{i}"
        both.add(key)
        (left if i % 2 == 0 else right).add(key)
    left.merge(right)
    assert left.to_bytes() == both.to_bytes()


def test_hyperloglog_estimate_and_merge():
    empty = HyperLogLog()
    assert empty.estimate() == 0
    assert empty.size_bytes == 1 << 14

    whole = HyperLogLog(12)
    left = HyperLogLog(12)
    right = HyperLogLog(12)
    count = 20_000
    for i in range(count):
        key = f"event-{i}"
        whole.add(key)
        (left if i % 2 == 0 else right).add(key)
    assert abs(whole.estimate() - count) / count < 0.05
    left.merge(right)
    assert left.to_bytes() == whole.to_bytes()
    assert HyperLogLog.from_bytes(whole.to_bytes()) == whole

    with pytest.raises(ValueError):
        HyperLogLog(2)
    with pytest.raises(ValueError):
        HyperLogLog.from_bytes(b"not-a-sketch")


def test_count_min_never_undercounts_and_merges():
    sized = CountMinSketch.from_error(0.01, 0.01)
    assert sized.width == 272
    assert sized.depth == 5

    left = CountMinSketch(64, 3)
    right = CountMinSketch(64, 3)
    left.add("alpha", 3)
    right.add("alpha", 4)
    left.merge(right)
    assert left.estimate("alpha") == 7

    truth = Counter()
    sketch = CountMinSketch(256, 4)
    for i in range(500):
        key = f"k{i % 25}"
        sketch.add(key, (i % 3) + 1)
        truth[key] += (i % 3) + 1
    for key, count in truth.items():
        assert sketch.estimate(key) >= count
    assert CountMinSketch.from_bytes(sketch.to_bytes()) == sketch


def test_reservoir_keeps_the_prefix_and_resumes():
    sample = Reservoir(4, seed=99)
    sample.update([10, 20, 30, 40])
    assert sample.sample() == [10, 20, 30, 40]
    assert sample.quantile(0.5) == 25
    assert sample.seen == 4

    original = Reservoir(8, seed=42)
    for i in range(100):
        original.add(float(i))
    resumed = Reservoir.from_bytes(original.to_bytes())
    for i in range(100, 160):
        original.add(float(i))
        resumed.add(float(i))
    assert resumed.sample() == original.sample()
    assert resumed.seen == original.seen

    with pytest.raises(ValueError):
        Reservoir(4).quantile(0.5)


def test_space_saving_bounds_and_replacement():
    summary = SpaceSaving(2)
    summary.update(["a", "a", "a", "b", "c"])
    assert summary.monitored("a")
    assert not summary.monitored("b")
    assert summary.upper_bound("a") == 3
    assert summary.lower_bound("c") == 1
    assert summary.upper_bound("b") == 2

    truth = Counter()
    stream = SpaceSaving(20)
    for i in range(1000):
        key = f"k{i % 50}"
        stream.add(key)
        truth[key] += 1
    assert sum(item.count for item in stream.top()) == 1000
    for key, count in truth.items():
        assert stream.lower_bound(key) <= count <= stream.upper_bound(key)

    top = stream.top(1)
    assert len(top) == 1
    key, count, error = top[0]
    assert key == top[0].key
    assert count == top[0].count
    assert error == top[0].error
    assert SpaceSaving.from_bytes(stream.to_bytes()) == stream


def test_card_cli_reports_exact_cardinality(tmp_path, capsys):
    path = tmp_path / "events.txt"
    path.write_text("a\nb\na\nc\n", encoding="utf-8")
    assert main(["card", str(path), "--precision", "14", "--exact"]) == 0
    output = capsys.readouterr().out
    assert "lines: 4" in output
    assert "exact: 3" in output
    assert "relative_error:" in output


def test_card_cli_reads_stdin(monkeypatch, capsys):
    monkeypatch.setattr("sys.stdin", io.StringIO("red\nred\nblue\n"))
    assert main(["card", "--exact", "--precision", "12"]) == 0
    output = capsys.readouterr().out
    assert "lines: 3" in output
    assert "exact: 2" in output


def test_freq_cli_lists_exact_counts_when_the_summary_fits(tmp_path, capsys):
    path = tmp_path / "events.txt"
    path.write_text("a\na\na\nb\nb\nc\n", encoding="utf-8")
    assert main(["freq", str(path), "--top", "3", "--counters", "10"]) == 0
    output = capsys.readouterr().out
    assert "1\ta\t3\t0\t3" in output
    assert "2\tb\t2\t0\t2" in output
    assert "3\tc\t1\t0\t1" in output


def test_quantiles_cli_matches_the_full_sample(tmp_path, capsys):
    path = tmp_path / "values.txt"
    path.write_text("".join(f"{i}\n" for i in range(1, 101)), encoding="utf-8")
    assert main(["quantiles", str(path), "--sample", "100", "--p", "0.5,0.9"]) == 0
    output = capsys.readouterr().out
    assert "seen: 100" in output
    assert "sample: 100" in output
    assert "p50: 50.5" in output
    assert "p90: 90.1" in output


def test_bloom_cli_answers_membership(tmp_path, capsys):
    members = tmp_path / "members.txt"
    queries = tmp_path / "queries.txt"
    members.write_text("a\nb\nc\n", encoding="utf-8")
    queries.write_text("a\nz\n", encoding="utf-8")
    probe = BloomFilter(10_000, 0.01)
    probe.update(["a", "b", "c"])
    assert "z" not in probe
    assert main(["bloom", str(members), "--query", str(queries), "--capacity", "10000"]) == 0
    output = capsys.readouterr().out
    assert "a\tyes" in output
    assert "z\tno" in output


def test_cli_rejects_a_missing_file_and_a_bad_precision(capsys):
    assert main(["card", "no-such-file.txt"]) == 1
    assert "no such file" in capsys.readouterr().err
    with pytest.raises(SystemExit) as caught:
        main(["card", "--precision", "2"])
    assert caught.value.code == 2
    assert main(["quantiles", "--p", "1.5"]) == 1
