"""Check how the session layer handles the shapes a driver may emit:
no-semicolon statements, blank lines, comment lines, and a wedged buffer."""
import os
import select
import shutil
import subprocess
import sys
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/minivectordb"
DATA = "/tmp/mvd_probe_data"


def read_line(p, timeout=1.5):
    r, _, _ = select.select([p.stdout], [], [], timeout)
    if not r:
        return None
    return p.stdout.readline().decode("utf-8", "replace").strip()


def case(name, lines):
    shutil.rmtree(DATA, ignore_errors=True)
    os.makedirs(DATA, exist_ok=True)
    p = subprocess.Popen([BIN, "--data-dir", DATA], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
    print(f"--- {name}")
    for sent in lines:
        p.stdin.write((sent + "\n").encode())
        p.stdin.flush()
        t0 = time.time()
        got = read_line(p)
        dt = (time.time() - t0) * 1000
        print(f"  send={sent!r:45} -> {got!r} ({dt:.0f}ms)")
    p.stdin.write(b"QUIT;\n")
    p.stdin.flush()
    p.wait(timeout=10)
    print(f"  exit={p.returncode}")


case("no semicolon", [
    "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2))",
    "INSERT INTO t VALUES (1, [1,2])",
    "SELECT * FROM t",
])
case("blank and comment lines", [
    "CREATE TABLE t (id BIGINT PRIMARY KEY);",
    "",
    "   ",
    "# note",
    "-- note",
    "SELECT 1;",
])
case("unbalanced then valid", [
    "CREATE TABLE t (id BIGINT PRIMARY KEY);",
    "SELECT * FROM t WHERE (id = 1;",
    "SELECT * FROM t;",
    "SELECT 1;",
])
shutil.rmtree(DATA, ignore_errors=True)
