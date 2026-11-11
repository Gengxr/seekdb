import json, subprocess, tempfile, shutil
BIN = "/data/henchi/projects/oceanbase_minivectordb_20261109/work/minivectordb/build/minivectordb"
class S:
    def __init__(self, d):
        self.p = subprocess.Popen([BIN, "--data-dir", d], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.n = 0
        assert json.loads(self.p.stdout.readline()) == {"type": "ready"}
    def q(self, sql, mode="exact"):
        self.n += 1
        self.p.stdin.write(json.dumps({"id": self.n, "op": "execute", "sql": sql, "search_mode": mode}) + "\n"); self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())
    def close(self):
        self.n += 1
        self.p.stdin.write(json.dumps({"id": self.n, "op": "shutdown"}) + "\n"); self.p.stdin.flush()
        self.p.wait(timeout=10)

def check(label, got, want):
    ok = got == want
    print(("PASS " if ok else "FAIL ") + label + " -> " + json.dumps(got, ensure_ascii=False))
    return ok

d = tempfile.mkdtemp()
s = S(d)
allok = True
allok &= check("create", s.q("CREATE TABLE t (id BIGINT PRIMARY KEY, v BIGINT);"), {"id":1,"ok":True})
allok &= check("int64 max", s.q("INSERT INTO t VALUES (9223372036854775807, 9223372036854775807);"), {"id":2,"ok":True,"affected_rows":1})
allok &= check("int64 min", s.q("INSERT INTO t VALUES (-9223372036854775808, -9223372036854775808);"), {"id":3,"ok":True,"affected_rows":1})
allok &= check("select bounds", s.q("SELECT id, v FROM t ORDER BY id;"), {"id":4,"ok":True,"columns":["id","v"],"rows":[[-9223372036854775808,-9223372036854775808],[9223372036854775807,9223372036854775807]]})
allok &= check("above max", s.q("INSERT INTO t VALUES (9223372036854775808, 1);"), {"id":5,"ok":False,"error":{"code":"integer_overflow","message":"integer literal out of range"}})
allok &= check("below min", s.q("INSERT INTO t VALUES (1, -9223372036854775809);"), {"id":6,"ok":False,"error":{"code":"integer_overflow","message":"integer literal out of range"}})
allok &= check("where min", s.q("SELECT id FROM t WHERE v = -9223372036854775808;"), {"id":7,"ok":True,"columns":["id"],"rows":[[-9223372036854775808]]})
allok &= check("where range", s.q("SELECT id FROM t WHERE v > -9223372036854775808 ORDER BY id;"), {"id":8,"ok":True,"columns":["id"],"rows":[[9223372036854775807]]})
allok &= check("update min", s.q("UPDATE t SET v = -9223372036854775808 WHERE id = 9223372036854775807;"), {"id":9,"ok":True,"affected_rows":1})
allok &= check("dup min pk", s.q("INSERT INTO t VALUES (-9223372036854775808, 0);"), {"id":10,"ok":False,"error":{"code":"duplicate_key","message":"duplicate primary key"}})
s.close()
s2 = S(d)
allok &= check("restart bounds", s2.q("SELECT id FROM t ORDER BY id;"), {"id":1,"ok":True,"columns":["id"],"rows":[[-9223372036854775808],[9223372036854775807]]})
s2.close(); shutil.rmtree(d, ignore_errors=True)
print("ALL", "PASS" if allok else "FAIL")
