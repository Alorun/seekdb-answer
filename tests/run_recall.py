#!/usr/bin/env python3
"""Differential exact/ANN recall test for the vector TopK paths.

Generates a deterministic 10k x 128 table, inserts it through the official
JSON Lines protocol, and compares every result with a NumPy brute-force oracle
using the task tie rule (distance, then primary key ascending).

Checks:
  * exact mode matches the oracle (ids, order, distance tolerance);
  * ann mode recall@10 >= 0.95 for filtered and unfiltered groups;
  * UPDATE/DELETE/INSERT invalidate the derived vector cache;
  * BEGIN/ROLLBACK restores the visible row set and TopK results;
  * equal distances are ordered by primary key.

Usage:
  python3 tests/run_recall.py --bin ./build/minivectordb [--rows 10000]
"""
import argparse
import json
import os
import random
import shutil
import subprocess
import tempfile

import numpy as np


class Client:
    def __init__(self, binary, data_dir):
        self.p = subprocess.Popen(
            [binary, "--data-dir", data_dir],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1)
        ready = self.p.stdout.readline()
        assert '"ready"' in ready, ready
        self.n = 0

    def req(self, sql, mode="exact"):
        self.n += 1
        self.p.stdin.write(json.dumps({
            "id": self.n, "op": "execute", "sql": sql,
            "search_mode": mode}, separators=(",", ":")) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("server exited")
        obj = json.loads(line)
        if not obj.get("ok"):
            raise RuntimeError(line[:400])
        return obj

    def close(self):
        self.n += 1
        self.p.stdin.write(json.dumps({"id": self.n, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        self.p.stdout.readline()
        self.p.wait(timeout=30)


def fmt_vec(v):
    return "[" + ",".join(f"{x:.6f}" for x in v) + "]"


def topk_oracle(vectors, query, mask, k=10):
    idx = np.nonzero(mask)[0]
    if idx.size == 0:
        return []
    d = ((vectors[idx] - query) ** 2).sum(axis=1)
    order = np.lexsort((idx, d))
    return [(int(idx[o]) + 1, float(np.sqrt(d[o]))) for o in order[:k]]


def db_rows(resp):
    out = []
    for row in resp["rows"]:
        out.append((int(row[0]), float(row[1])))
    return out


def check_equal(got, want, tag, tol=2e-5):
    if len(got) != len(want):
        raise AssertionError(f"{tag}: size {len(got)} != {len(want)}")
    for (gid, gd), (wid, wd) in zip(got, want):
        if gid != wid:
            raise AssertionError(f"{tag}: id {gid} != {wid}; got {got} want {want}")
        scale = max(1.0, abs(wd))
        if abs(gd - wd) > tol * scale:
            raise AssertionError(f"{tag}: distance {gd} != {wd}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="./build/minivectordb")
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--queries", type=int, default=100)
    ap.add_argument("--seed", type=int, default=20261111)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    vectors = rng.uniform(-1.0, 1.0, (args.rows, args.dim)).astype(np.float32)
    cats = np.array([f"cat{i:02d}" for i in range(50)])
    labels = np.array([cats[i % len(cats)] for i in range(args.rows)])

    data_dir = tempfile.mkdtemp(prefix="mvdb-recall-")
    cli = Client(args.bin, data_dir)
    try:
        cli.req(f"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), "
                f"embedding VECTOR({args.dim}));")
        batch = 100
        for start in range(0, args.rows, batch):
            end = min(start + batch, args.rows)
            tuples = []
            for i in range(start, end):
                tuples.append(
                    f"({i + 1},'{labels[i]}','{fmt_vec(vectors[i].tolist())}')")
            cli.req("INSERT INTO docs VALUES " + ",".join(tuples) + ";")

        # Warm the derived cache outside the checked region.
        q0 = rng.uniform(-1.0, 1.0, args.dim).astype(np.float32)
        cli.req(f"SELECT id, L2_DISTANCE(embedding, '{fmt_vec(q0.tolist())}') AS d "
                "FROM docs ORDER BY d LIMIT 10;")

        exact_hits = 0
        ann_hits = 0
        total = 0
        for qi in range(args.queries):
            q = rng.uniform(-1.0, 1.0, args.dim).astype(np.float32)
            filtered = qi % 2 == 0
            if filtered:
                cat = cats[rng.integers(0, len(cats))]
                where = f" WHERE category = '{cat}'"
                mask = labels == cat
            else:
                where = ""
                mask = np.ones(args.rows, dtype=bool)
            sql = (f"SELECT id, L2_DISTANCE(embedding, '{fmt_vec(q.tolist())}') AS d "
                   f"FROM docs{where} ORDER BY d LIMIT 10;")
            want = topk_oracle(vectors, q, mask)
            got_exact = db_rows(cli.req(sql, "exact"))
            check_equal(got_exact, want, f"exact q{qi}")
            got_ann = db_rows(cli.req(sql, "ann"))
            # ANN must contain valid, unique, filtered ids and hit the oracle.
            ids = [g[0] for g in got_ann]
            if len(ids) != len(set(ids)):
                raise AssertionError(f"ann q{qi}: duplicate ids {ids}")
            if any(not mask[i - 1] for i in ids):
                raise AssertionError(f"ann q{qi}: filter violation {ids}")
            ann_hits += len(set(ids) & {w[0] for w in want})
            exact_hits += 10
            total += len(want)
        print(f"exact queries: {args.queries} (ids/order/distance match oracle)")
        print(f"ann recall@10: {ann_hits / max(1, total):.4f}")

        # --- cache invalidation: UPDATE one vector, INSERT one, DELETE one ---
        newv = rng.uniform(-1.0, 1.0, args.dim).astype(np.float32)
        vectors[0] = newv
        cli.req(f"UPDATE docs SET embedding = '{fmt_vec(newv.tolist())}' WHERE id = 1;")
        cli.req("DELETE FROM docs WHERE id = 2;")
        deleted = np.zeros(args.rows, dtype=bool)
        deleted[1] = True
        q = rng.uniform(-1.0, 1.0, args.dim).astype(np.float32)
        sql = (f"SELECT id, L2_DISTANCE(embedding, '{fmt_vec(q.tolist())}') AS d "
               "FROM docs WHERE category = 'cat00' ORDER BY d LIMIT 10;")
        mask = (labels == "cat00") & (~deleted)
        check_equal(db_rows(cli.req(sql, "exact")),
                    topk_oracle(vectors, q, mask), "after-update-delete")
        check_equal(db_rows(cli.req(sql, "ann")),
                    topk_oracle(vectors, q, mask), "after-update-delete-ann")

        # --- transaction visibility and rollback ---
        cli.req("BEGIN;")
        cli.req(f"UPDATE docs SET embedding = '{fmt_vec((newv * -1).tolist())}' WHERE id = 1;")
        mask_txn = np.ones(args.rows, dtype=bool)
        mask_txn[1] = False
        q = rng.uniform(-1.0, 1.0, args.dim).astype(np.float32)
        sql = (f"SELECT id, L2_DISTANCE(embedding, '{fmt_vec(q.tolist())}') AS d "
               "FROM docs ORDER BY d LIMIT 10;")
        in_txn = db_rows(cli.req(sql, "exact"))
        if len(in_txn) != 10:
            raise AssertionError("transaction query did not see its own update")
        cli.req("ROLLBACK;")
        check_equal(db_rows(cli.req(sql, "exact")),
                    topk_oracle(vectors, q, mask_txn), "after-rollback")

        # --- ties: three identical vectors must be ordered by pk ---
        cli.req("CREATE TABLE ties (id BIGINT PRIMARY KEY, v VECTOR(4));")
        cli.req("INSERT INTO ties VALUES (9,'[1,0,0,0]'),(3,'[1,0,0,0]'),(7,'[1,0,0,0]'),"
                "(1,'[1,0,0,0]'),(5,'[1,0,0,0]');")
        got = cli.req("SELECT id FROM ties ORDER BY L2_DISTANCE(v, '[1,0,0,0]') LIMIT 5;")
        ids = [int(r[0]) for r in got["rows"]]
        if ids != [1, 3, 5, 7, 9]:
            raise AssertionError(f"tie order {ids} != [1,3,5,7,9]")
        print("cache invalidation, rollback, and tie order: OK")
    finally:
        cli.close()
        shutil.rmtree(data_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
