#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Focused reproducer for the case-4 hang (kill during recovery)."""
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/minivectordb"


def start(d):
    return subprocess.Popen([BIN, "--data-dir", d, "--cache-mb", "8"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def send(p, sql):
    p.stdin.write((sql + "\n").encode())
    p.stdin.flush()
    line = p.stdout.readline()
    return json.loads(line) if line else None


d = tempfile.mkdtemp(prefix="mvd-repro-")
data = os.path.join(d, "data")
os.makedirs(data)
p = start(data)
print("created", send(p, "CREATE TABLE bulk (id BIGINT PRIMARY KEY, content VARCHAR(32))"))
values = ",".join(f"({i},'x{i}')" for i in range(1000, 1600))
print("bulk insert", send(p, f"INSERT INTO bulk VALUES {values}"))
print("begin", send(p, "BEGIN"))
print("txn insert", send(p, f"INSERT INTO bulk VALUES {','.join(f'({i},chr{i})' for i in range(2000, 2100))}"))
print("commit", send(p, "COMMIT"))
os.kill(p.pid, signal.SIGKILL)
p.wait()
print("killed")

for i in range(3):
    q = subprocess.Popen([BIN, "--data-dir", data, "--cache-mb", "8"], stdin=subprocess.PIPE,
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    time.sleep(0.01)
    q.kill()
    out, err = q.communicate(timeout=30)
    print(f"quick kill {i}: rc={q.returncode} err={err.decode()[:200]!r}")

p = start(data)
print("restarted")
time.sleep(0.5)
print("query", send(p, "SELECT id FROM bulk WHERE id >= 2000 ORDER BY id"))
p.stdin.close()
p.wait(timeout=30)
print("done rc=", p.returncode, "stderr=", p.stderr.read().decode()[:300])
