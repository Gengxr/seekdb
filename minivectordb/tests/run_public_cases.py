#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Local driver for the public MiniVectorDB cases.

The public task package ships SQL scripts plus JSON assertions but no driver, so
this script plays the driver role: it starts the database with a data directory,
streams statements over stdin, and compares one JSON response per statement
against the expected assertion.

Usage:
    python3 run_public_cases.py --binary ./build/minivectordb --cases ../mysqltest \
        [--case sql_crud] [--keep] [--verbose]
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from decimal import Decimal

TIMEOUT_PER_STMT = 30.0
FLOAT_TOL = 1e-5


class Runner:
    def __init__(self, binary: str, data_dir: str, env: dict | None = None):
        self.binary = binary
        self.data_dir = data_dir
        self.env = env
        self.proc: subprocess.Popen | None = None
        self.stderr_path = os.path.join(data_dir, "stderr.log")

    def start(self) -> None:
        args = [self.binary, "--data-dir", self.data_dir]
        self.proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=open(self.stderr_path, "ab"),
            env=self.env,
        )

    def send(self, sql: str) -> dict:
        assert self.proc and self.proc.stdin and self.proc.stdout
        self.proc.stdin.write((sql + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        line = self._read_line()
        if line is None:
            raise RuntimeError("no response (process exited?)")
        try:
            return json.loads(line)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"non-JSON response: {line!r} ({exc})")

    def _read_line(self) -> str | None:
        assert self.proc and self.proc.stdout
        deadline = time.time() + TIMEOUT_PER_STMT
        buf = bytearray()
        fd = self.proc.stdout
        while True:
            if time.time() > deadline:
                raise RuntimeError("timeout waiting for response")
            ch = fd.readline()
            if not ch:
                return None
            return ch.decode("utf-8").rstrip("\n")

    def stop(self, graceful: bool = True) -> None:
        if not self.proc:
            return
        try:
            if graceful and self.proc.stdin:
                self.proc.stdin.close()
            self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        finally:
            if self.proc.stdout:
                self.proc.stdout.close()
            self.proc = None


def close_enough(actual, expected) -> bool:
    if isinstance(expected, bool) or isinstance(actual, bool):
        return actual == expected
    if isinstance(expected, (int, float)) and isinstance(actual, (int, float)):
        if isinstance(expected, int) and isinstance(actual, int):
            return actual == expected
        a, e = float(actual), float(expected)
        return abs(a - e) <= FLOAT_TOL or (e != 0 and abs(a - e) / abs(e) <= FLOAT_TOL)
    if isinstance(expected, str) and isinstance(actual, str):
        return actual == expected
    return actual == expected


def compare_rows(actual_rows, expected_rows, unordered: bool) -> tuple[bool, str]:
    if len(actual_rows) != len(expected_rows):
        return False, f"row count {len(actual_rows)} != {len(expected_rows)}"
    if not unordered:
        for i, (a, e) in enumerate(zip(actual_rows, expected_rows)):
            if len(a) != len(e):
                return False, f"row {i}: width {len(a)} != {len(e)}"
            for j, (av, ev) in enumerate(zip(a, e)):
                if not close_enough(av, ev):
                    return False, f"row {i} col {j}: {av!r} != {ev!r}"
        return True, ""
    remaining = list(expected_rows)
    for a in actual_rows:
        found = None
        for idx, e in enumerate(remaining):
            if len(a) != len(e):
                continue
            if all(close_enough(av, ev) for av, ev in zip(a, e)):
                found = idx
                break
        if found is None:
            return False, f"unexpected row {a!r}"
        remaining.pop(found)
    return True, ""


def check(actual: dict, expected: dict) -> tuple[bool, str]:
    if "error" in expected:
        if "error" not in actual:
            return False, f"expected error {expected['error']!r}, got {actual!r}"
        if not isinstance(actual["error"], str) or not actual["error"]:
            return False, f"error code must be a non-empty string, got {actual['error']!r}"
        if actual["error"] != expected["error"]:
            return False, f"error name {actual['error']!r} != {expected['error']!r} (semantic mismatch)"
        return True, ""
    if "error" in actual:
        return False, f"unexpected error {actual['error']!r}"
    if not expected:
        return True, ""
    if "affected_rows" in expected:
        if actual.get("affected_rows") != expected["affected_rows"]:
            return False, f"affected_rows {actual.get('affected_rows')} != {expected['affected_rows']}"
    if "columns" in expected:
        if actual.get("columns") != expected["columns"]:
            return False, f"columns {actual.get('columns')} != {expected['columns']}"
    if "rows" in expected:
        ok, why = compare_rows(
            actual.get("rows") or [], expected["rows"], bool(expected.get("unordered", False))
        )
        if not ok:
            return False, why
    return True, ""


def run_case(binary: str, cases_dir: str, case: dict, keep: bool, verbose: bool) -> tuple[bool, str]:
    test_path = os.path.join(cases_dir, case["test"])
    result_path = os.path.join(cases_dir, case["result"])
    with open(test_path, encoding="utf-8") as f:
        lines = [ln.rstrip("\n") for ln in f]
    with open(result_path, encoding="utf-8") as f:
        expected = json.load(f)

    workdir = tempfile.mkdtemp(prefix=f"mvd-{case['id']}-")
    data_dir = os.path.join(workdir, "data")
    os.makedirs(data_dir, exist_ok=True)
    runner = Runner(binary, data_dir)
    runner.start()
    failures = []
    idx = 0
    try:
        for raw in lines:
            line = raw.strip()
            if not line:
                continue
            if line == "--restart":
                runner.stop(graceful=True)
                runner.start()
                continue
            if line.startswith("--mode"):
                mode = line.split()[-1]
                runner.send(f"SET vector_mode = {mode}")
                continue
            if idx >= len(expected):
                failures.append(f"statement {idx} has no expected result: {line}")
                break
            try:
                actual = runner.send(line)
            except Exception as exc:  # noqa: BLE001
                failures.append(f"statement {idx} ({line}): {exc}")
                break
            ok, why = check(actual, expected[idx])
            if not ok:
                failures.append(f"statement {idx} ({line}): {why}")
            elif verbose:
                print(f"  ok {idx}: {line}")
            idx += 1
        if idx != len(expected):
            failures.append(f"executed {idx} statements, expected {len(expected)}")
    finally:
        runner.stop(graceful=True)
        if not keep:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"  data dir kept at {workdir}")
    if failures:
        return False, "; ".join(failures)
    return True, ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--cases", required=True, help="directory containing manifest.json")
    ap.add_argument("--case", default=None)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    manifest_path = os.path.join(args.cases, "manifest.json")
    with open(manifest_path, encoding="utf-8") as f:
        manifest = json.load(f)
    cases = manifest["cases"]
    if args.case:
        cases = [c for c in cases if c["id"] == args.case]
        if not cases:
            print(f"unknown case {args.case}", file=sys.stderr)
            return 2

    failed = 0
    for case in cases:
        ok, why = run_case(args.binary, args.cases, case, args.keep, args.verbose)
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {case['id']}" + ("" if ok else f"  <- {why}"))
        if not ok:
            failed += 1
    total = len(cases)
    print(f"\n{total - failed}/{total} cases passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
