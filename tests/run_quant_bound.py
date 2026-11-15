#!/usr/bin/env python3
"""Check the triangle-inequality superset when quantization scales differ.

q7 uses one table-wide scale, so ||v-q|| and s*||code(v)-code(q)|| share the
same scale and the bound |d - s*dq| <= err_v + err_q holds.  A per-vector scale
breaks that premise: the same code distance can map to different metric
distances.  This script quantifies the first ratio where a true top-10 member
falls outside the code-space superset.
"""
import argparse
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("--rows", type=int, default=10000)
ap.add_argument("--dim", type=int, default=128)
ap.add_argument("--queries", type=int, default=100)
ap.add_argument("--pairs", type=int, default=10000)
ap.add_argument("--seed", type=int, default=7)
A = ap.parse_args()
rng = np.random.default_rng(A.seed)
X = rng.uniform(-1, 1, (A.rows, A.dim)).astype(np.float32)
Q = rng.uniform(-1, 1, (A.queries, A.dim)).astype(np.float32)
base = np.abs(X).max() / 63.0
levels = 63

# Same scale: verify the bound on random pairs.
same_viol = 0
for _ in range(A.pairs):
    i = rng.integers(0, A.rows); j = rng.integers(0, A.rows)
    a, b = X[i], X[j]
    ca = np.rint(a / base).clip(-levels, levels)
    cb = np.rint(b / base).clip(-levels, levels)
    da = np.sqrt(((a - ca * base) ** 2).sum())
    db = np.sqrt(((b - cb * base) ** 2).sum())
    d = np.linalg.norm(a - b)
    dq = base * np.linalg.norm(ca - cb)
    if abs(d - dq) > da + db + 1e-4:
        same_viol += 1
print(f"bound same-scale pairs={A.pairs} violations={same_viol}")

for ratio in (1.0, 2.0, 4.0, 8.0):
    recalls = []
    fracs = []
    first_viol = None
    for qi in range(A.queries):
        q = Q[qi]
        scales = base * np.exp(rng.uniform(-np.log(ratio), np.log(ratio), A.rows)).astype(np.float32)
        C = np.rint(X / scales[:, None]).clip(-levels, levels)
        Dq = C * scales[:, None]
        err = np.sqrt(((X - Dq) ** 2).sum(1))
        cq = np.rint(q / base).clip(-levels, levels)
        # The fast kernel can only use one scale: it computes the code
        # distance in base-scale units even though each row was quantized
        # with its own scale.  That is the failure mode under test.
        dq = (base * (C - cq)) ** 2
        dq = dq.sum(1).astype(np.float64)
        dtrue = ((X - q) ** 2).sum(1)
        want = set(np.argsort(dtrue, kind="stable")[:10].tolist())
        kth = np.partition(dq, 9)[9]
        E = err.max() + base * np.linalg.norm(q / base - cq)
        thr = (np.sqrt(max(0.0, kth)) + 2.0 * E) ** 2 * (1 + 1e-6) + 1e-12
        cand = set(np.nonzero(dq <= thr)[0].tolist())
        hit = len(want & cand)
        recalls.append(hit / 10.0)
        fracs.append(len(cand) / A.rows)
        if first_viol is None and hit < 10:
            first_viol = qi
    print(f"recall@10 ratio={ratio:g} {np.mean(recalls):.3f} "
          f"candidates_mean={np.mean(fracs):.3f} candidates_max={np.max(fracs):.3f} "
          f"first_miss_query={first_viol}")
