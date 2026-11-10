"""Fuzz the session layer: every statement must produce exactly one line and
the process must survive arbitrary (also malformed) input."""
import random
import subprocess
import sys
import os
import shutil

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/minivectordb"
DATA = "/tmp/mvd_fuzz_data"

WORDS = [
    "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "TABLE", "VECTOR", "BIGINT",
    "VARCHAR", "PRIMARY", "KEY", "ORDER", "BY", "LIMIT", "AND", "OR", "WHERE",
    "L2_DISTANCE", "BEGIN", "COMMIT", "ROLLBACK", "DROP", "ALTER", "JOIN",
    "t", "v", "id", "1", "0", "-1", "3.5", "'abc'", "[1,2,3]", "[1.0,2.0]",
    "(", ")", ",", ";", "*", "=", "<", ">", "<=", ">=", "NULL", "NaN", "Infinity",
    "1e400", "999999999999999999999999", "\x00", "中文", "''", "[]", "[[0]]",
]


def random_statement(rng):
    n = rng.randint(1, 12)
    return " ".join(rng.choice(WORDS) for _ in range(n)) + rng.choice([";", "", ""])


def main():
    shutil.rmtree(DATA, ignore_errors=True)
    os.makedirs(DATA, exist_ok=True)
    rng = random.Random(20261110)
    stmts = []
    # Seed a real table so later statements can hit execution paths too.
    stmts.append("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(8), v VECTOR(3));")
    stmts.append("INSERT INTO t VALUES (1, 'a', [1.0, 2.0, 3.0]);")
    for _ in range(20000):
        stmts.append(random_statement(rng))
    stmts.append("QUIT;")

    p = subprocess.run(
        [BIN, "--data-dir", DATA],
        input=("\n".join(stmts) + "\n").encode("utf-8", "surrogateescape"),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=300,
    )
    out_lines = p.stdout.count(b"\n")
    print(f"exit={p.returncode} statements={len(stmts)} responses={out_lines}")
    if p.returncode != 0:
        print("STDERR tail:", p.stderr.decode("utf-8", "replace")[-2000:])
        print("FAIL: non-zero exit")
        return 1
    if out_lines != len(stmts) - 1:  # QUIT produces no response line
        print(f"FAIL: response count {out_lines} != {len(stmts) - 1}")
        # show first divergence
        lines = p.stdout.decode("utf-8", "replace").splitlines()
        for i, (s, l) in enumerate(zip(stmts, lines)):
            if i >= 5:
                print("sample", i, repr(s), "->", l[:120])
                break
        return 1
    print("PASS")
    shutil.rmtree(DATA, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
