#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Crash-recovery tests for MiniVectorDB.

The published crash model is "process is force-killed" (SIGKILL), so that is
what these cases do:

1. committed autocommit rows survive a SIGKILL;
2. an uncommitted explicit transaction leaves no trace after a SIGKILL;
3. a multi-row autocommit statement is all-or-nothing after a SIGKILL;
4. killing the process again while it is recovering still converges to a
   consistent state on the next start.
"""
from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time


class Db:
    def __init__(self, binary: str, data_dir: str):
        self.binary = binary
        self.data_dir = data_dir
        self.proc: subprocess.Popen | None = None

    def start(self) -> None:
        self.proc = subprocess.Popen(
            [self.binary, "--data-dir", self.data_dir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        line = self.proc.stdout.readline()
        if not line or json.loads(line) != {"type": "ready"}:
            raise RuntimeError("missing ready line: %r" % (line,))
        self.next_id = 1

    def send(self, sql: str, timeout: float = 30.0) -> dict:
        assert self.proc and self.proc.stdin and self.proc.stdout
        rid = self.next_id
        self.next_id += 1
        req = {"id": rid, "op": "execute", "sql": sql, "search_mode": "exact"}
        self.proc.stdin.write((json.dumps(req) + "\n").encode())
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("no response")
        resp = json.loads(line)
        if resp.get("id") != rid:
            raise RuntimeError("response id mismatch: %r" % (resp,))
        return resp

    def query(self, sql: str):
        r = self.send(sql)
        if not r.get("ok"):
            raise RuntimeError("query failed: %r" % (r,))
        return r.get("rows", [])

    def stop(self) -> None:
        if not self.proc:
            return
        rid = self.next_id
        self.next_id += 1
        self.proc.stdin.write((json.dumps({"id": rid, "op": "shutdown"}) + "\n").encode())
        self.proc.stdin.flush()
        self.proc.stdout.readline()
        self.proc.wait(timeout=60)
        self.proc = None

    def kill(self) -> None:
        assert self.proc
        os.kill(self.proc.pid, signal.SIGKILL)
        self.proc.wait(timeout=60)
        self.proc = None


def expect(cond, msg, failures):
    if not cond:
        failures.append(msg)
        print(f"  FAIL {msg}")
    else:
        print(f"  ok   {msg}")


def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else "build/minivectordb"
    workdir = tempfile.mkdtemp(prefix="mvd-crash-")
    data_dir = os.path.join(workdir, "data")
    os.makedirs(data_dir, exist_ok=True)
    failures: list[str] = []
    db = Db(binary, data_dir)

    print("case 1: committed rows survive SIGKILL")
    db.start()
    db.send("CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(32), embedding VECTOR(4))")
    for i in range(1, 51):
        db.send(f"INSERT INTO docs VALUES ({i},'row-{i}','[{i},0,0,0]')")
    db.kill()
    db.start()
    rows = db.query("SELECT id FROM docs ORDER BY id")
    expect([r[0] for r in rows] == list(range(1, 51)), "all 50 committed rows recovered", failures)

    print("case 2: uncommitted transaction leaves nothing behind")
    db.send("BEGIN")
    db.send("INSERT INTO docs VALUES (100,'uncommitted','[1,1,1,1]')")
    db.send("UPDATE docs SET content = 'pending' WHERE id = 1")
    pending = db.query("SELECT content FROM docs WHERE id = 1")
    expect(pending == [["pending"]], "transaction sees its own write", failures)
    db.kill()
    db.start()
    rows = db.query("SELECT id FROM docs WHERE id = 100")
    expect(rows == [], "uncommitted insert is gone", failures)
    rows = db.query("SELECT content FROM docs WHERE id = 1")
    expect(rows == [["row-1"]], "uncommitted update rolled back", failures)

    print("case 3: multi-row autocommit statement is atomic under SIGKILL")
    db.send("CREATE TABLE bulk (id BIGINT PRIMARY KEY, content VARCHAR(32))")
    values = ",".join(f"({i},'x{i}')" for i in range(1000, 1600))
    db.send(f"INSERT INTO bulk VALUES {values}")
    db.kill()
    db.start()
    rows = db.query("SELECT id FROM bulk ORDER BY id")
    ids = [r[0] for r in rows]
    expect(ids == list(range(1000, 1600)) or ids == [], f"bulk insert is all-or-nothing ({len(ids)} rows)", failures)

    print("case 4: kill during recovery converges")
    db.send("BEGIN")
    db.send("INSERT INTO bulk VALUES " + ",".join(f"({i},'chr{i}')" for i in range(2000, 2100)))
    db.send("COMMIT")
    db.kill()
    # start and kill immediately, repeatedly
    for _ in range(3):
        p = subprocess.Popen([binary, "--data-dir", data_dir], stdin=subprocess.PIPE,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.01)
        p.kill()
        p.wait()
    db.start()
    rows = db.query("SELECT id FROM bulk WHERE id >= 2000 ORDER BY id")
    expect([r[0] for r in rows] == list(range(2000, 2100)), "committed transaction survives repeated kill during recovery", failures)
    db.stop()

    shutil.rmtree(workdir, ignore_errors=True)
    print()
    if failures:
        print(f"FAILED {len(failures)} check(s)")
        return 1
    print("all crash-recovery checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
