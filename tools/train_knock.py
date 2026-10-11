#!/usr/bin/env python3
"""Train the badge's on-device accept rule and export it as fixed-point C.

    python tools/train_knock.py --data tools/knock_data_sisfall.csv
    python tools/train_knock.py --data tools/knock_data.csv tools/knock_data_sisfall.csv
    python tools/train_knock.py --data ... --out firmware/lib/BumpLogic/knock_model.h

WHAT IS BEING LEARNED
---------------------
Not the whole detector. Amplitude, isolation, agitation and the refractory stay
exactly as they are. The only thing replaced is the FINAL accept, which today
is two hand-tuned integer comparisons in bump_logic.cpp:

    accept = !(hot > dhot_max || qcons < dq_need)

In its place, a logistic regression over the five features the detector has
already computed by the time a candidate's ringdown window closes. Nothing new
is measured on the badge and no new state is kept -- this is a different
decision over the same numbers, which is why it costs ~20 lines of fixed-point
integer arithmetic and no floats.

WHY LOGISTIC REGRESSION AND NOT SOMETHING BIGGER
------------------------------------------------
Five features and (realistically) a few hundred labelled candidates. A linear
model is what that much data supports; a deeper model would fit the operator's
knocking habits rather than the physics. It also exports to five integers and a
shift, which is the entire reason it can run in the detector's hot path.

FEATURE SCALING IS DUPLICATED AND MUST MATCH
--------------------------------------------
bump_logic.cpp scales peak, qrun and agit by >> 4 before applying weights; hot
and qcons pass through. The same scaling is applied here. If these two ever
disagree the exported weights are silently meaningless, which is why the
constant appears in a comment on both sides.

HONEST LIMITS
-------------
  * The SisFall-derived rows are ALL NEGATIVE. Nobody knocked on anything in
    that dataset, so it cannot teach the model what a knock looks like -- only
    what real-world impacts that are NOT knocks look like. That is genuinely
    useful as a negative class and useless on its own.
  * A positive class has to come off the badge: `bump label knock`, `bump cap`,
    knock it thirty times, which is what tools/capture_knock.py automates.
  * This script REFUSES to export a model trained on one class. A model that
    has never seen a knock would classify everything as not-a-knock and look
    perfectly accurate while being worthless.

No scikit-learn: numpy only, so the repo keeps a single dependency and the
quantisation stays under this file's control.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

# Must match f_peak / f_qrun / f_agit in bump_logic.cpp.
PEAK_SHIFT = 4
QRUN_SHIFT = 4
AGIT_SHIFT = 4

FEATURES = ("peak", "hot", "qcons", "qrun", "agit")

# Fixed-point shift for the exported weights. 12 keeps a weight's resolution
# fine (1/4096) while the worst-case product stays far inside int32:
# |w| ~ 1e5 against a feature <= ~600 is ~6e7, summed over five terms ~3e8.
EXPORT_SHIFT = 12


def load(paths: list[Path]) -> tuple[np.ndarray, np.ndarray, list[str]]:
    """Read badge-schema candidate CSVs. Returns features, labels, label names."""
    rows: list[list[int]] = []
    names: list[str] = []
    for p in paths:
        if not p.exists():
            print(f"missing: {p}", file=sys.stderr)
            continue
        for line in p.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line.startswith("cand,") or line.startswith("cand,label"):
                continue
            f = line.split(",")
            if len(f) < 10:
                continue
            try:
                label = f[1]
                peak, hot, qcons, qrun, agit = (int(f[i]) for i in (2, 3, 4, 5, 6))
            except ValueError:
                continue
            rows.append([peak >> PEAK_SHIFT, hot, qcons,
                         qrun >> QRUN_SHIFT, agit >> AGIT_SHIFT])
            names.append(label)
    if not rows:
        return np.empty((0, 5)), np.empty(0), []
    X = np.asarray(rows, dtype=np.float64)
    # Anything labelled with "knock" is a deliberate strike; everything else --
    # slams, walking, desk noise, SisFall activities and falls -- is not.
    y = np.asarray([1.0 if "knock" in n.lower() else 0.0 for n in names])
    return X, y, names


def fit(X: np.ndarray, y: np.ndarray, iters: int = 4000, l2: float = 1e-3,
        seed: int = 0) -> tuple[np.ndarray, float]:
    """Logistic regression by gradient descent on standardised features.

    Standardising only matters for conditioning; the scaling is folded back
    into the raw-feature weights before they are returned, so the caller never
    sees it and the exported integers apply directly to the badge's features.
    """
    rng = np.random.default_rng(seed)
    mu = X.mean(axis=0)
    sd = X.std(axis=0)
    sd[sd < 1e-9] = 1.0                      # a constant feature carries no signal
    Z = (X - mu) / sd

    w = rng.normal(0.0, 0.01, size=Z.shape[1])
    b = 0.0
    n = len(y)
    # Class weighting, because a capture session yields far more rejected
    # candidates than knocks and an unweighted fit would just predict "no".
    pos = max(y.sum(), 1.0)
    neg = max(n - y.sum(), 1.0)
    sw = np.where(y > 0.5, n / (2 * pos), n / (2 * neg))

    lr = 0.5
    for _ in range(iters):
        p = 1.0 / (1.0 + np.exp(-(Z @ w + b)))
        g = (p - y) * sw
        gw = Z.T @ g / n + l2 * w
        gb = g.sum() / n
        w -= lr * gw
        b -= lr * gb

    # Fold standardisation back: w_raw = w/sd, b_raw = b - sum(w*mu/sd)
    w_raw = w / sd
    b_raw = b - float(np.sum(w * mu / sd))
    return w_raw, b_raw


def quantise(w: np.ndarray, b: float) -> tuple[list[int], int]:
    """Float weights -> integers for `(w0 + sum wi*fi) >> EXPORT_SHIFT > 0`."""
    k = float(1 << EXPORT_SHIFT)
    wi = [int(round(v * k)) for v in w]
    w0 = int(round(b * k))
    return [w0] + wi, EXPORT_SHIFT


def score_int(Xi: np.ndarray, weights: list[int], shift: int) -> np.ndarray:
    """Evaluate the exported integer model exactly as the firmware will."""
    w0 = weights[0]
    acc = np.full(len(Xi), w0, dtype=np.int64)
    for i in range(5):
        acc += weights[i + 1] * Xi[:, i].astype(np.int64)
    return acc >> shift


def report(tag: str, y: np.ndarray, pred: np.ndarray) -> dict:
    tp = int(((pred == 1) & (y == 1)).sum())
    fp = int(((pred == 1) & (y == 0)).sum())
    tn = int(((pred == 0) & (y == 0)).sum())
    fn = int(((pred == 0) & (y == 1)).sum())
    prec = tp / (tp + fp) if tp + fp else float("nan")
    rec = tp / (tp + fn) if tp + fn else float("nan")
    print(f"  {tag:<18} TP={tp:<4} FP={fp:<4} TN={tn:<4} FN={fn:<4} "
          f"precision={prec:.3f} recall={rec:.3f}")
    return {"tp": tp, "fp": fp, "tn": tn, "fn": fn}


def emit_header(path: Path, weights: list[int], shift: int, n: int,
                pos: int, sources: list[Path]) -> None:
    names = ", ".join(p.name for p in sources)
    with path.open("w", encoding="utf-8", newline="\n") as fh:
        fh.write("// GENERATED by tools/train_knock.py -- do not edit by hand.\n")
        fh.write("//\n")
        fh.write(f"// Trained on {n} labelled candidates ({pos} knocks) from: {names}\n")
        fh.write("//\n")
        fh.write("// Replaces the hand-tuned final accept in bump_logic.cpp:\n")
        fh.write("//     accept = !(hot > dhot_max || qcons < dq_need)\n")
        fh.write("// with a logistic regression over the five features already\n")
        fh.write("// computed when a candidate's ringdown window closes.\n")
        fh.write("//\n")
        fh.write("// Feature scaling MUST match f_peak/f_qrun/f_agit in\n")
        fh.write("// bump_logic.cpp: peak, qrun and agit are >> 4; hot and qcons\n")
        fh.write("// pass through. If they drift apart these weights are\n")
        fh.write("// meaningless and nothing will say so.\n")
        fh.write("//\n")
        fh.write("// To install: include this and assign it to the detector's\n")
        fh.write("// model, then `bump model on`. Both rules stay switchable so\n")
        fh.write("// the acceptance protocol can be run against each.\n")
        fh.write("#pragma once\n\n")
        fh.write('#include "bump_logic.h"\n\n')
        fh.write("namespace bump {\n\n")
        fh.write("constexpr Model kTrainedModel = {\n")
        fh.write(f"    /* w0      */ {weights[0]},\n")
        fh.write(f"    /* w_peak  */ {weights[1]},\n")
        fh.write(f"    /* w_hot   */ {weights[2]},\n")
        fh.write(f"    /* w_qcons */ {weights[3]},\n")
        fh.write(f"    /* w_qrun  */ {weights[4]},\n")
        fh.write(f"    /* w_agit  */ {weights[5]},\n")
        fh.write(f"    /* shift   */ {shift},\n")
        fh.write("    /* trained */ 1,\n")
        fh.write("};\n\n")
        fh.write("}  // namespace bump\n")
    print(f"\nexported -> {path}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", nargs="+", required=True, type=Path,
                    help="one or more candidate CSVs (badge capture and/or dataset)")
    ap.add_argument("--out", type=Path, default=None,
                    help="write the C header here (default: print only)")
    ap.add_argument("--iters", type=int, default=4000)
    args = ap.parse_args()

    X, y, names = load(args.data)
    if len(y) == 0:
        print("no candidate rows found. The CSV must have badge-schema rows "
              "starting with 'cand,'.", file=sys.stderr)
        return 2

    pos = int(y.sum())
    neg = len(y) - pos
    print(f"loaded {len(y)} candidates: {pos} knock, {neg} not-knock")
    by = {}
    for n in names:
        by[n] = by.get(n, 0) + 1
    print("  labels: " + "  ".join(f"{k}={v}" for k, v in
                                   sorted(by.items(), key=lambda kv: -kv[1])[:8]))

    # The guard that matters. One class means there is nothing to separate.
    if pos == 0 or neg == 0:
        missing = "knock" if pos == 0 else "not-knock"
        print(f"\nREFUSING TO TRAIN: no {missing} examples.", file=sys.stderr)
        print("A model fitted on one class predicts that class for everything "
              "and scores 100% while being worthless.", file=sys.stderr)
        print("\nThe SisFall rows are all negatives by construction -- nobody "
              "knocked in that dataset.", file=sys.stderr)
        print("Capture the positive class off the badge (~2 minutes):",
              file=sys.stderr)
        print("    python tools/capture_knock.py knock 45", file=sys.stderr)
        print("    python tools/capture_knock.py slam  45", file=sys.stderr)
        print("then pass both CSVs to --data.", file=sys.stderr)
        return 3

    Xi = X.astype(np.int64)
    w, b = fit(X, y, iters=args.iters)
    weights, shift = quantise(w, b)

    print("\nfloat weights (per scaled feature):")
    for name, v in zip(FEATURES, w):
        print(f"  {name:<6} {v:+.5f}")
    print(f"  bias   {b:+.5f}")

    print("\nhow the two rules compare on this data:")
    # The hand-tuned rule, recomputed from the same rows for a fair contrast.
    hand = ((Xi[:, 1] <= 8) & (Xi[:, 2] >= 4)).astype(int)
    report("hand-tuned", y, hand)
    pred_f = (1.0 / (1.0 + np.exp(-(X @ w + b))) > 0.5).astype(int)
    report("learned (float)", y, pred_f)
    pred_i = (score_int(Xi, weights, shift) > 0).astype(int)
    report("learned (int8/exported)", y, pred_i)

    # Quantisation is measured, not assumed.
    disagree = int((pred_i != pred_f).sum())
    print(f"\n  quantisation changed {disagree} of {len(y)} decisions "
          f"({100.0 * disagree / len(y):.1f}%)")
    if disagree:
        print("  Raise EXPORT_SHIFT if that is too high for comfort.")

    print("\nexported model:")
    print(f"  w0={weights[0]} w_peak={weights[1]} w_hot={weights[2]} "
          f"w_qcons={weights[3]} w_qrun={weights[4]} w_agit={weights[5]} "
          f"shift={shift}")

    if args.out:
        emit_header(args.out, weights, shift, len(y), pos, args.data)
    else:
        print("\n(no --out given, nothing written)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
