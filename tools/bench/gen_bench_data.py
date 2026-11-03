#!/usr/bin/env python3
"""Generate a CAGRA build/query benchmark dataset for seekdb lib=cuvs.

Outputs (all in --out):
  base.f32        n x dim float32, row major
  query.f32       nq x dim float32
  gt10.i32        nq x 10  exact L2 neighbours (base row ids)
  gt100.i32       nq x 100 exact L2 neighbours
  load.tsv        "id\t[v0,v1,...]" rows for LOAD DATA (float32 exact round-trip)
  first_query.txt "[v0,v1,...]" literal used as the timed first query
  meta.json       dataset description

Either pass --base-fbin (SIFT/FBIN: int32 n, int32 dim, float32 data) and
optionally --query-fbin, or let the generator synthesise clustered data.
"""

import argparse
import json
import pathlib
import sys

import numpy as np

GT_TOPK = 100


def read_fbin(path):
    with open(path, "rb") as handle:
        header = np.fromfile(handle, dtype=np.int32, count=2)
        if header.size != 2:
            raise SystemExit(f"invalid fbin header: {path}")
        rows, dim = int(header[0]), int(header[1])
        data = np.fromfile(handle, dtype=np.float32, count=rows * dim)
    if data.size != rows * dim:
        raise SystemExit(f"truncated fbin: {path} ({data.size} != {rows * dim})")
    return data.reshape(rows, dim)


def synth(rows, dim, seed, kind):
    rng = np.random.default_rng(seed)
    if kind == "uniform":
        return (rng.random((rows, dim), dtype=np.float32) * 1000.0).astype(np.float32)
    # Clustered: a realistic-ish mix of Gaussian blobs plus background noise.
    clusters = max(8, dim // 8)
    centers = (rng.random((clusters, dim), dtype=np.float32) * 800.0).astype(np.float32)
    assignment = rng.integers(0, clusters, size=rows)
    noise = rng.normal(scale=25.0, size=(rows, dim)).astype(np.float32)
    data = centers[assignment] + noise
    background = rng.integers(0, rows, size=rows // 20)
    data[background] = (rng.random((background.size, dim), dtype=np.float32) * 1000.0)
    return np.clip(data, 0.0, None).astype(np.float32)


def exact_gt(base, queries, topk, block=256):
    ground = np.empty((queries.shape[0], topk), dtype=np.int32)
    base_sq = np.einsum("ij,ij->i", base, base).astype(np.float32)
    for start in range(0, queries.shape[0], block):
        chunk = queries[start:start + block]
        dist = (
            np.einsum("ij,ij->i", chunk, chunk)[:, None]
            - 2.0 * (chunk @ base.T)
            + base_sq[None, :]
        )
        part = np.argpartition(dist, topk - 1, axis=1)[:, :topk]
        picked = np.take_along_axis(dist, part, axis=1)
        order = np.argsort(picked, axis=1, kind="stable")
        ground[start:start + chunk.shape[0]] = np.take_along_axis(part, order, axis=1)
    return ground


def integral(values):
    sample = values.reshape(-1)[: 1 << 20]
    return bool(np.all(sample == np.floor(sample)))


def write_rows(path, base, fmt):
    """Write `id<TAB>[v0,v1,...]` rows so float32 values round-trip exactly."""
    rows, dim = base.shape
    with open(path, "w", buffering=1 << 22) as out:
        for start in range(0, rows, 4096):
            block = base[start:start + 4096]
            cells = np.char.mod(fmt, block)
            joined = cells[:, 0]
            for col in range(1, dim):
                joined = np.char.add(np.char.add(joined, ","), cells[:, col])
            text = np.char.add(np.char.add("[", joined), "]")
            ids = np.char.add(
                np.char.add(np.arange(start, start + block.shape[0]).astype(str), "\t"), text
            )
            out.write("\n".join(ids.tolist()))
            out.write("\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--rows", type=int, default=250_000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--nq", type=int, default=4096)
    parser.add_argument("--seed", type=int, default=1314)
    parser.add_argument("--kind", choices=["clustered", "uniform"], default="clustered")
    parser.add_argument("--base-fbin", default="")
    parser.add_argument("--query-fbin", default="")
    parser.add_argument("--no-jitter", action="store_true",
                        help="use real query fbin values verbatim (no probe jitter)")
    args = parser.parse_args()

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    if args.base_fbin:
        base = read_fbin(args.base_fbin)
        if base.shape[0] > args.rows:
            base = base[: args.rows]
        rows, dim = base.shape
        if args.query_fbin:
            queries = read_fbin(args.query_fbin)[: args.nq].copy()
            jitter = False
        else:
            queries = None
        source = {"base_fbin": args.base_fbin, "query_fbin": args.query_fbin}
    else:
        rows, dim = args.rows, args.dim
        base = synth(rows, dim, args.seed, args.kind)
        queries = None
        source = {"kind": args.kind, "seed": args.seed}

    rng = np.random.default_rng(args.seed + 1)
    if queries is None:
        picks = rng.choice(rows, size=min(args.nq, rows), replace=False)
        queries = base[picks].copy()
        queries += rng.normal(scale=12.0, size=queries.shape).astype(np.float32)
        jitter = True
    if jitter and not args.no_jitter:
        # probes sit next to real points so the workload is non-trivial
        queries = (queries + rng.normal(scale=1.0, size=queries.shape).astype(np.float32)).astype(np.float32)

    print(f"[gen] base={base.shape} query={queries.shape} exact top-{GT_TOPK}", flush=True)
    ground = exact_gt(base, queries, GT_TOPK)

    base.tofile(out / "base.f32")
    queries.tofile(out / "query.f32")
    ground[:, :10].astype(np.int32).tofile(out / "gt10.i32")
    ground.astype(np.int32).tofile(out / "gt100.i32")

    fmt = "%.0f" if integral(base) and integral(queries) else "%.9g"
    print(f"[gen] writing load.tsv with format {fmt}", flush=True)
    write_rows(out / "load.tsv", base, fmt)
    (out / "first_query.txt").write_text(
        "[" + ",".join(fmt % value for value in queries[0]) + "]\n"
    )
    (out / "meta.json").write_text(json.dumps({
        "rows": int(rows), "dim": int(dim), "nq": int(queries.shape[0]),
        "topk": GT_TOPK, "format": fmt, "source": source,
    }, indent=2) + "\n")
    print(f"[gen] done: {out}", flush=True)


if __name__ == "__main__":
    sys.exit(main())
