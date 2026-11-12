#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MiniVectorDB performance harness for the JSON-lines protocol.

The judge speaks the JSON request/response protocol from the task package
(strictly serial: send one request, wait for its response).  The old bench.py
sent bare SQL lines and therefore deadlocks against the current main.cpp, so
this script reimplements the same published workload on top of the real
protocol:

  10,000 rows x 128 dimensions, written in batches of 100 rows per statement
  with a commit per batch, then 1,000 TopK queries (K=10) of which half carry
  an equality filter on a scalar column.

Reported per run:
  t_write_s      wall clock for the write phase (create table + all batches)
  t_query_s      wall clock for the 1,000 queries
  cpu_write_s    child CPU time (utime+stime) consumed by the write phase
  cpu_query_s    child CPU time consumed by the query phase
  peak_rss_mb    VmHWM of the child
  write_bytes_mb / wchar_mb   from /proc/<pid>/io
  data_dir_mb    size of the on-disk data directory

`t_*` include the Python side of the round trip, which is exactly what the
judge measures, while `cpu_*` isolates the database's own cost.

Usage:
  python3 bench_json.py --binary ./build/minivectordb [--repeat 3]
"""
from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


class Session:
    """Strictly serial JSON-lines client, mirroring the judge transport."""

    def __init__(self, binary: str, data_dir: str, stderr_path: str | None = None):
        err = open(stderr_path, "wb") if stderr_path else subprocess.DEVNULL
        self.proc = subprocess.Popen(
            [binary, "--data-dir", data_dir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=err,
            bufsize=1 << 16,
        )
        self._close_err = err if stderr_path else None
        self.sequence = 0
        self.startup_s = 0.0

    def wait_ready(self) -> None:
        t0 = time.perf_counter()
        line = self.proc.stdout.readline()
        self.startup_s = time.perf_counter() - t0
        message = json.loads(line)
        if message != {"type": "ready"}:
            raise RuntimeError(f"expected ready, got {message!r}")

    def request(self, sql: str | None = None, mode: str = "exact", op: str = "execute"):
        self.sequence += 1
        request = {"id": self.sequence, "op": op}
        if sql is not None:
            request["sql"] = sql
            request["search_mode"] = mode
        self.proc.stdin.write((json.dumps(request, ensure_ascii=False) + "\n").encode())
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("database exited before response")
        return json.loads(line)

    def shutdown(self) -> None:
        response = self.request(op="shutdown")
        if response.get("ok") is not True:
            raise RuntimeError(f"shutdown failed: {response!r}")
        self.proc.stdin.close()
        self.proc.wait(timeout=120)

    def cpu_seconds(self) -> float:
        try:
            with open(f"/proc/{self.proc.pid}/stat", encoding="utf-8") as f:
                fields = f.read().rsplit(")", 1)[1].split()
            ticks = os.sysconf("SC_CLK_TCK")
            return (int(fields[11]) + int(fields[12])) / ticks
        except OSError:
            return 0.0

    def peak_rss_mb(self) -> float:
        try:
            with open(f"/proc/{self.proc.pid}/status", encoding="utf-8") as f:
                for line in f:
                    if line.startswith("VmHWM:"):
                        return int(line.split()[1]) / 1024.0
        except OSError:
            pass
        return 0.0

    def io_counters(self) -> dict:
        result = {}
        try:
            with open(f"/proc/{self.proc.pid}/io", encoding="utf-8") as f:
                for line in f:
                    key, value = line.split(":")
                    result[key] = int(value)
        except OSError:
            pass
        return result

    def close(self) -> None:
        try:
            if self.proc.poll() is None:
                self.proc.kill()
            self.proc.wait(timeout=30)
        except Exception:
            pass
        if self._close_err:
            self._close_err.close()


def gen_rows(n: int, dim: int, seed: int, categories: int = 20):
    rng = random.Random(seed)
    rows = []
    for i in range(n):
        pk = i + 1
        category = f"c{rng.randrange(categories)}"
        vector = [round(rng.uniform(-1.0, 1.0), 4) for _ in range(dim)]
        rows.append((pk, category, vector))
    return rows


def vec_literal(values) -> str:
    return "[" + ",".join(f"{x:g}" for x in values) + "]"


def run_once(binary: str, args, rows, workdir: str, keep: bool = False) -> dict:
    data_dir = os.path.join(workdir, "data")
    os.makedirs(data_dir, exist_ok=True)
    result = {}

    t_launch = time.perf_counter()
    session = Session(binary, data_dir, os.path.join(workdir, "stderr.log"))
    try:
        session.wait_ready()
        result["t_startup_s"] = round(session.startup_s, 4)
        del t_launch

        autocommit = args.write_mode == "autocommit"
        session.request(
            f"CREATE TABLE bench (id BIGINT PRIMARY KEY, category VARCHAR(16), "
            f"payload VARCHAR(32), embedding VECTOR({args.dim}))"
        )

        io0 = session.io_counters()
        batch_sql = []
        for start in range(0, len(rows), args.batch):
            chunk = rows[start:start + args.batch]
            values = ",".join(
                f"({pk},'{cat}','row-{pk}',{vec_literal(vec)})" for pk, cat, vec in chunk
            )
            batch_sql.append(f"INSERT INTO bench VALUES {values}")

        cpu0 = session.cpu_seconds()
        t0 = time.perf_counter()
        for sql in batch_sql:
            if not autocommit:
                session.request("BEGIN")
            response = session.request(sql)
            if response.get("ok") is not True:
                raise RuntimeError(f"insert failed: {response!r}")
            if not autocommit:
                response = session.request("COMMIT")
                if response.get("ok") is not True:
                    raise RuntimeError(f"commit failed: {response!r}")
        result["t_write_s"] = round(time.perf_counter() - t0, 4)
        result["cpu_write_s"] = round(session.cpu_seconds() - cpu0, 4)
        result["peak_rss_after_write_mb"] = round(session.peak_rss_mb(), 2)
        io1 = session.io_counters()

        rng = random.Random(args.seed + 1)
        queries = []
        for i in range(args.queries):
            source = rows[rng.randrange(len(rows))]
            query = [x + rng.uniform(-0.05, 0.05) for x in source[2]]
            category = rows[rng.randrange(len(rows))][1] if i % 2 == 0 else None
            queries.append((query, category))

        sqls = []
        for query, category in queries:
            where = f" WHERE category = '{category}'" if category else ""
            sqls.append(
                f"SELECT id, L2_DISTANCE(embedding, '{vec_literal(query)}') AS distance "
                f"FROM bench{where} ORDER BY distance LIMIT {args.k}"
            )

        cpu0 = session.cpu_seconds()
        t0 = time.perf_counter()
        returned = []
        for sql in sqls:
            response = session.request(sql)
            if response.get("ok") is not True:
                raise RuntimeError(f"query failed: {response!r}")
            returned.append([row[0] for row in response["rows"]])
        result["t_query_s"] = round(time.perf_counter() - t0, 4)
        result["cpu_query_s"] = round(session.cpu_seconds() - cpu0, 4)
        io2 = session.io_counters()

        result["peak_rss_mb"] = round(session.peak_rss_mb(), 2)
        result["wchar_mb"] = round(io2.get("wchar", 0) / 1048576.0, 2)
        result["write_bytes_mb"] = round(io2.get("write_bytes", 0) / 1048576.0, 2)
        result["rchar_mb"] = round(io2.get("rchar", 0) / 1048576.0, 2)
        result["read_bytes_mb"] = round(io2.get("read_bytes", 0) / 1048576.0, 2)
        result["queries_per_s"] = round(args.queries / result["t_query_s"], 1)
        result["data_dir_mb"] = round(
            sum(os.path.getsize(os.path.join(data_dir, f)) for f in os.listdir(data_dir))
            / 1048576.0,
            2,
        )
        del io0, io1

        if not args.skip_recall:
            import numpy as np

            vectors = np.asarray([r[2] for r in rows], dtype="float32")
            categories = [r[1] for r in rows]
            hits = total = 0
            for (query, category), got in zip(queries, returned):
                if category is None:
                    mask = None
                    subset = vectors
                else:
                    idx = [i for i, c in enumerate(categories) if c == category]
                    subset = vectors[idx]
                    mask = idx
                diff = subset.astype("float64") - np.asarray(query, dtype="float64")
                dist = np.sqrt((diff * diff).sum(axis=1))
                order = sorted(range(len(dist)), key=lambda t: (dist[t], (mask[t] if mask else t) + 1))
                truth = [(mask[t] + 1) if mask else (t + 1) for t in order[: args.k]]
                hits += len(set(truth) & set(got))
                total += len(truth)
            result["recall_at_k"] = round(hits / total, 5) if total else None

        session.shutdown()
    finally:
        session.close()

    if not keep:
        shutil.rmtree(workdir, ignore_errors=True)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--queries", type=int, default=1000)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--batch", type=int, default=100)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--write-mode", choices=("autocommit", "txn"), default="autocommit")
    parser.add_argument("--skip-recall", action="store_true")
    parser.add_argument("--json-out", default=None)
    parser.add_argument("--data-root", default="/data/henchi/tmp",
                        help="parent directory for the scratch data dir; use /dev/shm to "
                             "take fsync latency out of the picture")
    args = parser.parse_args()

    binary = os.path.abspath(args.binary)
    rows = gen_rows(args.rows, args.dim, args.seed)

    runs = []
    for attempt in range(args.repeat):
        workdir = tempfile.mkdtemp(prefix="mvd-bench-", dir=args.data_root)
        print(f"run {attempt + 1}/{args.repeat} ...", file=sys.stderr, flush=True)
        runs.append(run_once(binary, args, rows, workdir))

    report = {"binary": binary, "runs": runs}
    numeric = [k for k, v in runs[0].items() if isinstance(v, (int, float))]
    median = {}
    for key in numeric:
        values = [r[key] for r in runs if isinstance(r.get(key), (int, float))]
        if values:
            median[key] = round(statistics.median(values), 4)
    report["median"] = median
    print(json.dumps(report, indent=2, ensure_ascii=False))
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2, ensure_ascii=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
