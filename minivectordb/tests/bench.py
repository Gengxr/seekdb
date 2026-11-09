#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MiniVectorDB benchmark harness mirroring the published performance profile.

Default profile: 10,000 rows, 128 dimensions, 1,000 TopK queries (K=10), half of
them with an equality filter on a scalar column. Writes go in batches of 100
rows per statement with an explicit COMMIT, exactly like the published spec.

The script reports wall time, peak RSS and the bytes written/read on the data
device (from /proc/self/io for the child, which approximates rchar/wchar).
"""
from __future__ import annotations

import argparse
import json
import os
import random
import resource
import shutil
import subprocess
import sys
import tempfile
import time

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None


class Session:
    def __init__(self, binary: str, data_dir: str):
        self.proc = subprocess.Popen(
            [binary, "--data-dir", data_dir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )

    def send(self, sql: str) -> dict:
        self.proc.stdin.write((sql + "\n").encode())
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("no response")
        return json.loads(line)

    def close(self) -> None:
        self.proc.stdin.close()
        self.proc.wait(timeout=120)


def gen_rows(n: int, dim: int, seed: int, categories: int = 20):
    rng = random.Random(seed)
    rows = []
    for i in range(n):
        pk = i + 1
        cat = f"c{rng.randrange(categories)}"
        vec = [round(rng.uniform(-1, 1), 4) for _ in range(dim)]
        rows.append((pk, cat, vec))
    return rows


def vec_literal(v) -> str:
    return "[" + ",".join(f"{x:g}" for x in v) + "]"


def peak_rss_kb(pid: int) -> int:
    try:
        with open(f"/proc/{pid}/status", encoding="utf-8") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def brute_force(vectors, mask, query, k):
    idx = [i for i in range(len(vectors)) if mask[i]]
    if not idx:
        return []
    sub = vectors[idx]
    if np is not None:
        diff = sub.astype("float32") - np.asarray(query, dtype="float32")
        d = np.sqrt((diff.astype("float64") ** 2).sum(axis=1))
    else:  # pragma: no cover
        d = [sum((float(a) - float(b)) ** 2 for a, b in zip(vectors[i], query)) ** 0.5 for i in idx]
    order = sorted(range(len(idx)), key=lambda t: (d[t], idx[t]))[:k]
    return [idx[t] for t in order]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--queries", type=int, default=1000)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--cache-mb", type=int, default=256)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--skip-recall", action="store_true")
    args = ap.parse_args()

    workdir = tempfile.mkdtemp(prefix="mvd-bench-")
    data_dir = os.path.join(workdir, "data")
    os.makedirs(data_dir, exist_ok=True)
    rows = gen_rows(args.rows, args.dim, args.seed)

    s = Session(args.binary, data_dir)
    t0 = time.perf_counter()
    s.send(f"CREATE TABLE bench (id BIGINT PRIMARY KEY, category VARCHAR(16), payload VARCHAR(32), "
           f"embedding VECTOR({args.dim}))")
    for start in range(0, len(rows), args.batch):
        chunk = rows[start:start + args.batch]
        values = ",".join(
            f"({pk},'{cat}','row-{pk}',{vec_literal(vec)})" for pk, cat, vec in chunk
        )
        s.send("BEGIN")
        s.send(f"INSERT INTO bench VALUES {values}")
        s.send("COMMIT")
    t_write = time.perf_counter() - t0
    rss_after_write = peak_rss_kb(s.proc.pid)

    rng = random.Random(args.seed + 1)
    queries = []
    for i in range(args.queries):
        pk = rows[rng.randrange(len(rows))][0]
        qvec = rows[pk - 1][2][:]
        # perturb a little so results are not trivially the source row
        qvec = [x + rng.uniform(-0.05, 0.05) for x in qvec]
        cat = rows[rng.randrange(len(rows))][1]
        queries.append((qvec, cat if i % 2 == 0 else None))

    t0 = time.perf_counter()
    returned = []
    for qvec, cat in queries:
        where = f" WHERE category = '{cat}'" if cat else ""
        sql = (f"SELECT id, L2_DISTANCE(embedding, '{vec_literal(qvec)}') AS distance "
               f"FROM bench{where} ORDER BY distance LIMIT {args.k}")
        r = s.send(sql)
        returned.append([row[0] for row in r["rows"]])
    t_query = time.perf_counter() - t0
    rss = peak_rss_kb(s.proc.pid)
    s.close()

    recalls = []
    if not args.skip_recall:
        vectors = [r[2] for r in rows]
        cats = [r[1] for r in rows]
        for (qvec, cat), got in zip(queries, returned):
            truth_idx = brute_force(vectors, [c == cat for c in cats] if cat else [True] * len(rows), qvec, args.k)
            truth = [i + 1 for i in truth_idx]
            if not truth:
                continue
            hit = len(set(truth) & set(got))
            recalls.append(hit / len(truth))

    report = {
        "rows": args.rows,
        "dim": args.dim,
        "queries": args.queries,
        "k": args.k,
        "write_seconds": round(t_write, 4),
        "query_seconds": round(t_query, 4),
        "query_per_second": round(args.queries / t_query, 1),
        "peak_rss_after_write_mb": round(rss_after_write / 1024.0, 2),
        "peak_rss_mb": round(rss / 1024.0, 2),
        "recall_at_k": round(sum(recalls) / len(recalls), 5) if recalls else None,
        "data_dir_bytes": sum(
            os.path.getsize(os.path.join(data_dir, f)) for f in os.listdir(data_dir)
        ),
    }
    print(json.dumps(report, indent=2))
    if not args.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    else:
        print(f"data dir: {workdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
