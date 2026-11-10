#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Randomised differential test: MiniVectorDB vs an independent Python model.

The model implements the semantics written in the task statement (not the
implementation's behaviour), so a mismatch points at a bug in the database:

* L2_DISTANCE is the Euclidean distance computed from the stored float32 vector;
* filtered TopK is evaluated inside the filtered set;
* ties on distance (and on ORDER BY keys) break by primary key ascending;
* results without ORDER BY are output in primary key order;
* VARCHAR is compared by Unicode code point;
* autocommit per statement, explicit transactions with read-your-writes;
* a normal restart keeps committed data and drops uncommitted work.

Usage: python3 differential_test.py --binary ./build/minivectordb [--rounds 30] [--seed 1]
"""
from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

F32 = struct.Struct("<f")


def f32(x: float) -> float:
    return F32.unpack(F32.pack(x))[0]


def l2(a, b):
    return sum((float(x) - float(y)) ** 2 for x, y in zip(a, b)) ** 0.5


class Db:
    def __init__(self, binary: str, data_dir: str):
        self.binary = binary
        self.data_dir = data_dir
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            [self.binary, "--data-dir", self.data_dir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )

    def send(self, sql: str) -> dict:
        self.proc.stdin.write((sql + "\n").encode())
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError(f"no response for: {sql}")
        return json.loads(line)

    def query(self, sql: str):
        r = self.send(sql)
        if "error" in r:
            raise RuntimeError(f"unexpected error {r['error']} for: {sql}")
        return r["rows"]

    def stop(self):
        if self.proc:
            self.proc.stdin.close()
            self.proc.wait(timeout=60)
            self.proc = None

    def kill(self):
        if self.proc:
            self.proc.kill()
            self.proc.wait(timeout=60)
            self.proc = None


class Model:
    """Reference semantics for one table."""

    def __init__(self, dim: int):
        self.dim = dim
        self.rows: dict[int, dict] = {}
        self.txn: dict[int, dict | None] | None = None  # pk -> row or None (deleted)

    def begin(self):
        assert self.txn is None
        self.txn = {}

    def commit(self):
        if self.txn is not None:
            for pk, row in self.txn.items():
                if row is None:
                    self.rows.pop(pk, None)
                else:
                    self.rows[pk] = row
        self.txn = None

    def rollback(self):
        self.txn = None

    def visible(self) -> dict[int, dict]:
        view = dict(self.rows)
        if self.txn:
            for pk, row in self.txn.items():
                if row is None:
                    view.pop(pk, None)
                else:
                    view[pk] = row
        return view

    def insert(self, rows):
        seen = set()
        for row in rows:
            pk = row["id"]
            if pk in seen or pk in self.visible():
                raise KeyError("duplicate_key")
            seen.add(pk)
        for row in rows:
            if self.txn is not None:
                self.txn[row["id"]] = dict(row)
            else:
                self.rows[row["id"]] = dict(row)

    def delete(self, pred) -> int:
        victims = [pk for pk, row in self.visible().items() if pred(row)]
        for pk in victims:
            if self.txn is not None:
                self.txn[pk] = None
            else:
                self.rows.pop(pk, None)
        return len(victims)

    def update(self, pred, changes) -> int:
        victims = [(pk, dict(row)) for pk, row in self.visible().items() if pred(row)]
        for pk, row in victims:
            row.update(changes)
            if self.txn is not None:
                self.txn[pk] = row
            else:
                self.rows[pk] = row
        return len(victims)

    def select(self, where=None, order=None, limit=None, dist=None, cat_filter=None):
        rows = []
        for pk, row in self.visible().items():
            if where and not where(row):
                continue
            rec = dict(row)
            if dist is not None:
                rec["_d"] = l2([f32(v) for v in row["vec"]], [f32(v) for v in dist])
            rows.append(rec)
        if order == "distance":
            rows.sort(key=lambda r: (r["_d"], r["id"]))
        elif order == "value":
            rows.sort(key=lambda r: (r["value"], r["id"]))
        elif order == "-value":
            rows.sort(key=lambda r: (-r["value"], r["id"]))
        else:
            rows.sort(key=lambda r: r["id"])
        if limit is not None:
            rows = rows[:limit]
        return rows


def vec_literal(vec):
    return "[" + ",".join(f"{v:.6g}" for v in vec) + "]"


def compare_rows(actual, expected, tol=1e-5):
    if len(actual) != len(expected):
        return f"row count {len(actual)} != {len(expected)}"
    for i, (a, e) in enumerate(zip(actual, expected)):
        if len(a) != len(e):
            return f"row {i} width {len(a)} != {len(e)}"
        for j, (av, ev) in enumerate(zip(a, e)):
            if isinstance(ev, float):
                if abs(float(av) - ev) > tol and abs(float(av) - ev) > tol * max(1.0, abs(ev)):
                    return f"row {i} col {j}: {av!r} != {ev!r}"
            elif av != ev:
                return f"row {i} col {j}: {av!r} != {ev!r}"
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--rounds", type=int, default=30)
    ap.add_argument("--steps", type=int, default=40)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    workdir = tempfile.mkdtemp(prefix="mvd-diff-")
    data_dir = os.path.join(workdir, "data")
    os.makedirs(data_dir, exist_ok=True)
    db = Db(args.binary, data_dir)
    db.start()
    dim = rng.choice([2, 3, 4])
    cats = ["alpha", "beta", "gamma", "中", "é"]
    db.send(f"CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), value BIGINT, vec VECTOR({dim}))")
    model = Model(dim)
    failures = []
    next_pk = [1]

    def rand_row():
        pk = next_pk[0]
        next_pk[0] += 1
        return {
            "id": pk,
            "cat": rng.choice(cats),
            "value": rng.randrange(-50, 50),
            "vec": [round(rng.uniform(-3, 3), 3) for _ in range(dim)],
        }

    def row_sql(r):
        return f"({r['id']},'{r['cat']}',{r['value']},'{vec_literal(r['vec'])}')"

    def check(label, actual, expected):
        why = compare_rows(actual, expected)
        if why:
            failures.append(f"[{label}] {why}")

    for step in range(args.steps):
        op = rng.choices(
            ["insert", "update", "delete", "select_pk", "select_filter", "topk", "topk_filter",
             "txn", "restart", "update_vec"],
            weights=[18, 10, 8, 8, 8, 12, 12, 10, 4, 10],
        )[0]
        try:
            if op == "insert":
                rows = [rand_row() for _ in range(rng.randint(1, 4))]
                sql = "INSERT INTO t VALUES " + ",".join(row_sql(r) for r in rows)
                r = db.send(sql)
                if "error" in r:
                    failures.append(f"insert failed: {r} for {sql}")
                else:
                    model.insert(rows)
                    if r.get("affected_rows") != len(rows):
                        failures.append(f"insert affected_rows {r.get('affected_rows')} != {len(rows)}")

            elif op in ("update", "update_vec"):
                if not model.visible():
                    continue
                pk = rng.choice(list(model.visible()))
                if op == "update":
                    newval = rng.randrange(-50, 50)
                    r = db.send(f"UPDATE t SET value = {newval} WHERE id = {pk}")
                    changed = {"value": newval}
                else:
                    newcat = rng.choice(cats)
                    r = db.send(f"UPDATE t SET cat = '{newcat}' WHERE id = {pk}")
                    changed = {"cat": newcat}
                if "error" in r:
                    failures.append(f"update failed: {r}")
                else:
                    n = model.update(lambda row, pk=pk: row["id"] == pk, changed)
                    if r.get("affected_rows") != n:
                        failures.append(f"update affected_rows {r.get('affected_rows')} != {n}")

            elif op == "delete":
                if not model.visible():
                    continue
                threshold = rng.randrange(-50, 50)
                r = db.send(f"DELETE FROM t WHERE value < {threshold}")
                if "error" in r:
                    failures.append(f"delete failed: {r}")
                else:
                    n = model.delete(lambda row: row["value"] < threshold)
                    if r.get("affected_rows") != n:
                        failures.append(f"delete affected_rows {r.get('affected_rows')} != {n}")

            elif op == "select_pk":
                limit = rng.choice([None, 1, 5, 100])
                sql = "SELECT id, cat, value FROM t ORDER BY id" + (f" LIMIT {limit}" if limit else "")
                expected = [[r["id"], r["cat"], r["value"]] for r in model.select(limit=limit)]
                check(f"select_pk lim={limit}", db.query(sql), expected)

            elif op == "select_filter":
                cat = rng.choice(cats)
                sql = f"SELECT id FROM t WHERE cat = '{cat}' ORDER BY id"
                expected = [[r["id"]] for r in model.select(where=lambda row: row["cat"] == cat)]
                check(f"select_filter cat={cat}", db.query(sql), expected)

            elif op == "topk":
                q = [round(rng.uniform(-3, 3), 3) for _ in range(dim)]
                k = rng.choice([1, 2, 5, 20])
                sql = (f"SELECT id, L2_DISTANCE(vec, '{vec_literal(q)}') AS d FROM t "
                       f"ORDER BY d LIMIT {k}")
                expected = [[r["id"], r["_d"]] for r in model.select(order="distance", limit=k, dist=q)]
                check(f"topk k={k} q={q}", db.query(sql), expected)

            elif op == "topk_filter":
                cat = rng.choice(cats)
                q = [round(rng.uniform(-3, 3), 3) for _ in range(dim)]
                k = rng.choice([1, 3, 10])
                sql = (f"SELECT id, L2_DISTANCE(vec, '{vec_literal(q)}') AS d FROM t "
                       f"WHERE cat = '{cat}' ORDER BY d LIMIT {k}")
                expected = [[r["id"], r["_d"]] for r in
                            model.select(where=lambda row: row["cat"] == cat, order="distance",
                                         limit=k, dist=q)]
                check(f"topk_filter cat={cat} k={k}", db.query(sql), expected)

            elif op == "txn":
                db.send("BEGIN")
                model.begin()
                inner = rng.choice(["insert", "update", "delete", "rollback", "commit"])
                if inner == "insert":
                    rows = [rand_row() for _ in range(rng.randint(1, 3))]
                    db.send("INSERT INTO t VALUES " + ",".join(row_sql(r) for r in rows))
                    try:
                        model.insert(rows)
                    except KeyError:
                        pass
                    # read-your-writes check inside the transaction
                    ids = sorted(model.visible())
                    check("txn read own inserts",
                          db.query("SELECT id FROM t ORDER BY id"), [[i] for i in ids])
                    db.send("COMMIT")
                    model.commit()
                elif inner == "update":
                    if model.visible():
                        pk = rng.choice(list(model.visible()))
                        db.send(f"UPDATE t SET value = 777 WHERE id = {pk}")
                        model.update(lambda row, pk=pk: row["id"] == pk, {"value": 777})
                    db.send("COMMIT")
                    model.commit()
                elif inner == "delete":
                    if model.visible():
                        pk = rng.choice(list(model.visible()))
                        db.send(f"DELETE FROM t WHERE id = {pk}")
                        model.delete(lambda row, pk=pk: row["id"] == pk)
                    db.send("ROLLBACK")
                    model.rollback()
                elif inner == "rollback":
                    db.send("ROLLBACK")
                    model.rollback()
                else:
                    db.send("COMMIT")
                    model.commit()
                ids = sorted(model.visible())
                check("after txn", db.query("SELECT id FROM t ORDER BY id"), [[i] for i in ids])

            elif op == "restart":
                db.stop()
                db.start()
                ids = sorted(model.visible())
                check("after restart", db.query("SELECT id FROM t ORDER BY id"), [[i] for i in ids])

        except Exception as exc:  # noqa: BLE001
            failures.append(f"step {step} op {op}: {exc}")
            break

        if failures:
            break

    # final full comparison
    ids = sorted(model.visible())
    check("final ids", db.query("SELECT id FROM t ORDER BY id"), [[i] for i in ids])
    q = [0.0] * dim
    k = len(ids) + 3
    expected = [[r["id"], r["_d"]] for r in model.select(order="distance", limit=k, dist=q)]
    check("final topk", db.query(
        f"SELECT id, L2_DISTANCE(vec, '{vec_literal(q)}') AS d FROM t ORDER BY d LIMIT {k}"), expected)

    db.stop()
    if not args.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    else:
        print("data dir:", workdir)

    if failures:
        print("FAILURES:")
        for f in failures[:20]:
            print("  -", f)
        return 1
    print(f"differential test passed ({args.steps} steps, {args.rounds} rounds requested, seed {args.seed})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
