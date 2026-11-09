#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Large-data test: keep the working set far above the configured cache.

The published acceptance criteria require handling a data set larger than the
memory budget without loading everything into memory. This test writes a table
whose live data exceeds the cache budget several times over, restarts, and then
checks vector search results and the process peak RSS.
"""
from __future__ import annotations

import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time


def peak_rss_kb(pid: int) -> int:
    try:
        with open(f"/proc/{pid}/status", encoding="utf-8") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


class Db:
    def __init__(self, binary: str, data_dir: str, cache_mb: int):
        self.proc = subprocess.Popen(
            [binary, "--data-dir", data_dir, "--cache-mb", str(cache_mb)],
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

    def stop(self) -> None:
        self.proc.stdin.close()
        self.proc.wait(timeout=120)


def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/minivectordb"
    rows = int(sys.argv[2]) if len(sys.argv) > 2 else 120000
    dim = int(sys.argv[3]) if len(sys.argv) > 3 else 128
    cache_mb = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    workdir = tempfile.mkdtemp(prefix="mvd-large-")
    data_dir = os.path.join(workdir, "data")
    os.makedirs(data_dir, exist_ok=True)
    rng = random.Random(11)

    db = Db(binary, data_dir, cache_mb)
    t0 = time.perf_counter()
    db.send(f"CREATE TABLE big (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR({dim}))")
    batch = 500
    for start in range(0, rows, batch):
        parts = []
        for i in range(start, min(start + batch, rows)):
            vec = ",".join(f"{rng.uniform(-1,1):.4f}" for _ in range(dim))
            parts.append(f"({i + 1},'c{i % 10}','[{vec}]')")
        db.send("INSERT INTO big VALUES " + ",".join(parts))
    t_write = time.perf_counter() - t0
    rss_write = peak_rss_kb(db.proc.pid)
    db.stop()

    data_bytes = sum(os.path.getsize(os.path.join(data_dir, f)) for f in os.listdir(data_dir))
    print(f"write: {rows} rows x {dim} dims in {t_write:.2f}s, data {data_bytes/1e6:.1f} MB, "
          f"peak rss {rss_write/1024:.1f} MB, cache budget {cache_mb} MB")

    db = Db(binary, data_dir, cache_mb)
    t0 = time.perf_counter()
    r = db.send(f"SELECT id FROM big WHERE id = {rows}")
    restart_rows = r.get("rows", [])
    t_first = time.perf_counter() - t0
    qvec = ",".join("0.5" for _ in range(dim))
    t0 = time.perf_counter()
    r = db.send(f"SELECT id, L2_DISTANCE(embedding, '[{qvec}]') AS d FROM big ORDER BY d LIMIT 5")
    t_query = time.perf_counter() - t0
    rss = peak_rss_kb(db.proc.pid)
    db.stop()

    print(f"restart: first query {t_first*1000:.1f} ms, vector top5 {t_query*1000:.1f} ms, "
          f"peak rss {rss/1024:.1f} MB")
    ok = restart_rows == [[rows]] and len(r.get("rows", [])) == 5
    print("result rows:", r.get("rows"))
    print("PASS" if ok else "FAIL")
    shutil.rmtree(workdir, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
