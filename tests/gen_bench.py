#!/usr/bin/env python3
"""Generate a deterministic 10k x 128 benchmark for MiniVectorDB."""
import argparse
import math
import random


def fmt_vec(rng, dim):
    # ~[-1, 1) floats with 6 decimals, like a normalised embedding.
    vals = [rng.uniform(-1.0, 1.0) for _ in range(dim)]
    return "[" + ",".join(f"{v:.6f}" for v in vals) + "]"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--queries", type=int, default=1000)
    ap.add_argument("--categories", type=int, default=50)
    ap.add_argument("--content-len", type=int, default=64)
    ap.add_argument("--filter", choices=["category", "idrange", "none"],
                    default="category")
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--out", default="/tmp/mvdb-perf/data")
    args = ap.parse_args()

    import os
    os.makedirs(args.out, exist_ok=True)
    rng = random.Random(args.seed)
    cats = [f"cat{i:02d}" for i in range(args.categories)]

    with open(os.path.join(args.out, "schema.sql"), "w") as f:
        f.write("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), "
                f"content VARCHAR(1024), embedding VECTOR({args.dim}));\n")

    with open(os.path.join(args.out, "inserts.sql"), "w") as f:
        row = 0
        while row < args.rows:
            n = min(args.batch, args.rows - row)
            tuples = []
            for k in range(n):
                i = row + k + 1
                cat = cats[i % len(cats)]
                content = (f"document-{i:06d}-" + "x" * max(0, args.content_len - 16))
                tuples.append(f"({i},'{cat}','{content}','{fmt_vec(rng, args.dim)}')")
            f.write("INSERT INTO docs VALUES " + ",".join(tuples) + ";\n")
            row += n

    nq = args.queries
    half = nq // 2
    with open(os.path.join(args.out, "queries.sql"), "w") as f:
        for q in range(nq):
            v = fmt_vec(rng, args.dim)
            if args.filter == "category" and q < half:
                cat = cats[rng.randrange(len(cats))]
                f.write(f"SELECT id, L2_DISTANCE(embedding, '{v}') AS dist FROM docs "
                        f"WHERE category = '{cat}' ORDER BY dist LIMIT 10;\n")
            elif args.filter == "idrange" and q < half:
                lo = rng.randrange(1, args.rows - args.rows // 4)
                hi = lo + args.rows // 4
                f.write(f"SELECT id, L2_DISTANCE(embedding, '{v}') AS dist FROM docs "
                        f"WHERE id >= {lo} AND id < {hi} ORDER BY dist LIMIT 10;\n")
            else:
                f.write(f"SELECT id, L2_DISTANCE(embedding, '{v}') AS dist FROM docs "
                        f"ORDER BY dist LIMIT 10;\n")

    # ORDER BY id LIMIT 10 queries (secondary scored shape).
    with open(os.path.join(args.out, "orderby_id.sql"), "w") as f:
        for _ in range(nq):
            f.write("SELECT id, category FROM docs ORDER BY id LIMIT 10;\n")


if __name__ == "__main__":
    main()
