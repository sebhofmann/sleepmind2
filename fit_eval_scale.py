#!/usr/bin/env python3
"""Fit the eval -> expected-score sigmoid scale from training data.

Reads training data lines in the format produced by training_main:

    <fen> | <eval_cp_white_relative> | <wdl_white_relative>

and fits  P(score) = sigmoid((eval - a) / s)  by maximum likelihood
(cross-entropy, draws count as soft target 0.5).

The fitted scale s is directly comparable to bullet's `eval_scale`
(currently 400.0 in training/training.rs). The offset a should come out
near 0 for symmetric data; the no-offset fit is what bullet actually uses.

Usage:
    python3 fit_eval_scale.py [file-or-dir ...]

Defaults to ./training_data. Directories are scanned recursively for
regular files; gzip files (.gz) are supported.
"""

import glob
import gzip
import io
import math
import os
import sys
import time

import numpy as np

REFERENCE_SCALE = 400.0  # bullet eval_scale the net was trained with
PROGRESS_EVERY = 1_000_000  # lines between progress updates


def iter_files(args):
    paths = args if args else ["training_data"]
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                for n in sorted(names):
                    yield os.path.join(root, n)
        else:
            matched = glob.glob(p)
            if not matched:
                print(f"Warnung: {p} nicht gefunden", file=sys.stderr)
            yield from sorted(matched)


def fmt_duration(seconds):
    seconds = int(seconds)
    if seconds >= 3600:
        return f"{seconds // 3600}h{(seconds % 3600) // 60:02d}m"
    if seconds >= 60:
        return f"{seconds // 60}m{seconds % 60:02d}s"
    return f"{seconds}s"


def aggregate(files):
    """Bin positions by integer eval: eval -> (count, sum of scores)."""
    files = list(files)
    total_bytes = sum(os.path.getsize(p) for p in files if os.path.isfile(p))
    done_bytes = 0
    start = time.monotonic()

    def progress(cur_file_idx, path, file_bytes):
        frac = (done_bytes + file_bytes) / total_bytes if total_bytes else 1.0
        frac = min(frac, 1.0)
        elapsed = time.monotonic() - start
        eta = elapsed / frac - elapsed if frac > 0 else 0
        print(f"\r[{cur_file_idx}/{len(files)}] {os.path.basename(path)}: "
              f"{frac * 100:5.1f} %, {total_lines / max(elapsed, 1e-9) / 1e6:.1f} M Zeilen/s, "
              f"ETA {fmt_duration(eta)}   ", end="", file=sys.stderr, flush=True)

    counts = {}
    total_lines = 0
    bad_lines = 0
    used_files = 0
    for idx, path in enumerate(files, 1):
        try:
            raw = open(path, "rb")
        except OSError as e:
            print(f"\nWarnung: {path}: {e}", file=sys.stderr)
            continue
        if path.endswith(".gz"):
            f = io.TextIOWrapper(gzip.GzipFile(fileobj=raw), errors="replace")
        else:
            f = io.TextIOWrapper(raw, errors="replace")
        with raw, f:
            file_lines = 0
            for line in f:
                total_lines += 1
                if total_lines % PROGRESS_EVERY == 0:
                    progress(idx, path, raw.tell())
                parts = line.split("|")
                if len(parts) != 3:
                    bad_lines += 1
                    continue
                try:
                    ev = int(parts[1])
                    score = float(parts[2])
                except ValueError:
                    bad_lines += 1
                    continue
                if not 0.0 <= score <= 1.0:
                    bad_lines += 1
                    continue
                c = counts.get(ev)
                if c is None:
                    counts[ev] = [1, score]
                else:
                    c[0] += 1
                    c[1] += score
                file_lines += 1
            # for .gz files the compressed size counts towards total_bytes
            done_bytes += os.path.getsize(path) if os.path.isfile(path) else 0
            progress(idx, path, 0)
            if file_lines:
                used_files += 1
    elapsed = time.monotonic() - start
    print(f"\nFertig gelesen in {fmt_duration(elapsed)}.", file=sys.stderr)
    return counts, total_lines, bad_lines, used_files


def fit_logistic(x, n, y, with_offset):
    """Weighted logistic regression via Newton-Raphson.

    Minimizes sum n_i * CE(y_i, sigmoid(w0 + w1 * x_i)); x in pawns for
    conditioning. Returns (w0, w1). With with_offset=False, w0 is fixed at 0.
    """
    w0, w1 = 0.0, 1.0 / (REFERENCE_SCALE / 100.0)  # start at the reference curve
    for _ in range(100):
        z = w0 + w1 * x
        p = 1.0 / (1.0 + np.exp(-z))
        r = n * (p - y)
        h = n * p * (1.0 - p) + 1e-12
        if with_offset:
            g = np.array([r.sum(), (r * x).sum()])
            H = np.array([[h.sum(), (h * x).sum()],
                          [(h * x).sum(), (h * x * x).sum()]])
            d0, d1 = np.linalg.solve(H, g)
        else:
            d0 = 0.0
            d1 = (r * x).sum() / (h * x * x).sum()
        w0 -= d0
        w1 -= d1
        if abs(d0) < 1e-12 and abs(d1) < 1e-12:
            break
    return w0, w1


def cross_entropy(x, n, y, w0, w1):
    z = np.clip(w0 + w1 * x, -30, 30)
    p = 1.0 / (1.0 + np.exp(-z))
    ce = -(y * np.log(p) + (1.0 - y) * np.log(1.0 - p))
    return (n * ce).sum() / n.sum()


def main():
    counts, total, bad, used_files = aggregate(iter_files(sys.argv[1:]))
    if not counts:
        print("Keine verwertbaren Daten gefunden.", file=sys.stderr)
        sys.exit(1)

    evals = np.array(sorted(counts), dtype=np.float64)
    n = np.array([counts[int(e)][0] for e in evals], dtype=np.float64)
    y = np.array([counts[int(e)][1] / counts[int(e)][0] for e in evals])
    x = evals / 100.0  # pawns

    npos = int(n.sum())
    print(f"Dateien: {used_files}, Positionen: {npos:,}, "
          f"verworfene Zeilen: {bad:,} von {total:,}")
    print(f"Eval-Bereich: [{int(evals.min())}, {int(evals.max())}] cp, "
          f"mittlerer Score: {float((n * y).sum() / n.sum()):.4f}")
    print()

    w0f, w1f = fit_logistic(x, n, y, with_offset=False)
    scale_fixed = 100.0 / w1f
    w0o, w1o = fit_logistic(x, n, y, with_offset=True)
    scale_off = 100.0 / w1o
    offset_cp = -w0o / w1o * 100.0

    ce_ref = cross_entropy(x, n, y, 0.0, 100.0 / REFERENCE_SCALE)
    ce_fix = cross_entropy(x, n, y, w0f, w1f)
    ce_off = cross_entropy(x, n, y, w0o, w1o)

    print(f"Fit ohne Offset (entspricht bullet):  s = {scale_fixed:7.1f} cp")
    print(f"Fit mit Offset:                       s = {scale_off:7.1f} cp, "
          f"Offset a = {offset_cp:+.1f} cp")
    print(f"Referenz (bullet eval_scale):         s = {REFERENCE_SCALE:7.1f} cp")
    print()
    print(f"Cross-Entropy: Referenz {ce_ref:.6f} | Fit {ce_fix:.6f} | "
          f"Fit+Offset {ce_off:.6f}")
    rel = (scale_fixed / REFERENCE_SCALE - 1.0) * 100.0
    print(f"Abweichung von {REFERENCE_SCALE:.0f}: {rel:+.1f} %")
    print()

    # Calibration table: empirical winrate per 100cp bin vs both curves.
    print(f"{'Eval-Bin':>14} {'N':>10} {'empirisch':>10} "
          f"{'sig(e/400)':>10} {'Fit':>10}")
    edges = np.arange(-1000, 1001, 100)
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (evals >= lo) & (evals < hi)
        if n[m].sum() < 50:
            continue
        nb = n[m].sum()
        emp = (n[m] * y[m]).sum() / nb
        mid = (lo + hi) / 2.0
        p_ref = 1.0 / (1.0 + math.exp(-mid / REFERENCE_SCALE))
        p_fit = 1.0 / (1.0 + math.exp(-(w0f + w1f * mid / 100.0)))
        print(f"[{lo:+5d},{hi:+5d}) {int(nb):>10,} {emp:>10.3f} "
              f"{p_ref:>10.3f} {p_fit:>10.3f}")


if __name__ == "__main__":
    main()
