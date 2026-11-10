#!/usr/bin/env python3
"""Manual protocol probes for behaviour the public cases do not cover.

Talks JSON Lines to the built binary, in the same shape as the judge driver.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

BIN = sys.argv[1] if len(sys.argv) > 1 else "/usr/local/bin/minivectordb"


class Session:
    def __init__(self, data_dir):
        self.data_dir = data_dir
        self.proc = subprocess.Popen(
            [BIN, "--data-dir", data_dir],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        line = self.proc.stdout.readline()
        self.ready = json.loads(line)
        assert self.ready == {"type": "ready"}, line
        self.next_id = 1

    def request(self, sql, mode="exact"):
        rid = self.next_id
        self.next_id += 1
        req = {"id": rid, "op": "execute", "sql": sql, "search_mode": mode}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            err = self.proc.stderr.read()
            raise RuntimeError("no response; stderr=%s" % err)
        resp = json.loads(line)
        assert resp["id"] == rid, (resp, rid)
        return resp

    def shutdown(self):
        rid = self.next_id
        self.next_id += 1
        self.proc.stdin.write(json.dumps({"id": rid, "op": "shutdown"}) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        resp = json.loads(line)
        rest = self.proc.stdout.read()
        code = self.proc.wait(timeout=10)
        return resp, rest, code


def show(name, resp):
    print("%-34s %s" % (name, json.dumps(resp, ensure_ascii=False)[:300]))


def main():
    root = tempfile.mkdtemp(prefix="mvd-probe-")
    data = os.path.join(root, "data")
    os.makedirs(data)
    ok = True
    s = Session(data)
    try:
        show("create", s.request("CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(3));"))
        show("insert", s.request("INSERT INTO t VALUES (1, '[1,2,3]'), (2, '[2,2,2]');"))
        show("ann mode explicit", s.request("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0,0]') LIMIT 2;", "ann"))
        show("set vector_mode", s.request("SET vector_mode = ann;"))
        show("create index", s.request("CREATE VECTOR INDEX idx ON t(v);"))
        show("ann after index", s.request("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0,0]') LIMIT 2;", "ann"))
        show("nan vector", s.request("INSERT INTO t VALUES (3, '[nan,1,1]');"))
        show("inf vector", s.request("INSERT INTO t VALUES (4, '[1,inf,1]');"))
        show("overflow vector", s.request("INSERT INTO t VALUES (5, '[1e39,1,1]');"))
        show("dim mismatch", s.request("INSERT INTO t VALUES (6, '[1,2]');"))
        show("text parse", s.request("SELECT id FROM t WHERE id >= 1 ORDER BY id;"))
        show("select star", s.request("SELECT * FROM t ORDER BY id LIMIT 1;"))
        show("select vector", s.request("SELECT v FROM t WHERE id = 1;"))
        show("order by dist no limit", s.request("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0,0]');"))
        show("empty result", s.request("SELECT id FROM t WHERE id > 100;"))
        show("update none", s.request("UPDATE t SET id = 9 WHERE id > 100;"))
        resp, rest, code = s.shutdown()
        show("shutdown", resp)
        print("extra stdout after shutdown: %r  exit=%s" % (rest, code))
        if rest.strip() or code != 0:
            ok = False
    finally:
        pass
    # restart on the same data dir: committed rows must be there
    s2 = Session(data)
    try:
        show("after restart", s2.request("SELECT id FROM t ORDER BY id;"))
        resp, rest, code = s2.shutdown()
        print("restart shutdown: %s extra=%r exit=%s" % (json.dumps(resp), rest, code))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("PROBES DONE")


if __name__ == "__main__":
    main()
