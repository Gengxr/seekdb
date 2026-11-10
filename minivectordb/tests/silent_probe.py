"""Find inputs the session layer silently drops (a dropped statement would
desynchronise any driver that expects one response line per statement)."""
import random
import subprocess
import sys
import os
import select
import shutil

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/minivectordb"
DATA = "/tmp/mvd_silent_data"

WORDS = [
    "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "TABLE", "VECTOR", "BIGINT",
    "VARCHAR", "PRIMARY", "KEY", "ORDER", "BY", "LIMIT", "AND", "OR", "WHERE",
    "L2_DISTANCE", "BEGIN", "COMMIT", "ROLLBACK", "DROP", "ALTER", "JOIN",
    "t", "v", "id", "1", "0", "-1", "3.5", "'abc'", "[1,2,3]", "[1.0,2.0]",
    "(", ")", ",", ";", "*", "=", "<", ">", "<=", ">=", "NULL", "NaN", "Infinity",
    "1e400", "999999999999999999999999", "\x00", "中文", "''", "[]", "[[0]]",
    "#", "--", "/*", "*/", "\\q",
]


def gen(rng, n=600):
    out = []
    for _ in range(n):
        k = rng.randint(1, 8)
        out.append(" ".join(rng.choice(WORDS) for _ in range(k)) + rng.choice([";", "", ""]))
    return out


def main():
    shutil.rmtree(DATA, ignore_errors=True)
    os.makedirs(DATA, exist_ok=True)
    rng = random.Random(7)
    stmts = ["CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(8), v VECTOR(3));",
             "INSERT INTO t VALUES (1, 'a', [1.0, 2.0, 3.0]);"] + gen(rng)

    p = subprocess.Popen([BIN, "--data-dir", DATA], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
    silent = []
    for s in stmts:
        p.stdin.write((s + "\n").encode("utf-8", "surrogateescape"))
        p.stdin.flush()
        r, _, _ = select.select([p.stdout], [], [], 1.0)
        if not r:
            silent.append(s)
            continue
        p.stdout.readline()
    p.stdin.write(b"QUIT;\n")
    p.stdin.flush()
    p.wait(timeout=10)
    print(f"total={len(stmts)} silent={len(silent)} exit={p.returncode}")
    for s in silent[:25]:
        print("  silent:", repr(s))
    shutil.rmtree(DATA, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
