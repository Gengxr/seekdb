import json, os, subprocess, sys, tempfile, shutil

BIN = os.environ.get("MVD_BIN", "/data/henchi/projects/oceanbase_minivectordb_20261109/work/minivectordb/build/minivectordb")

class S:
    def __init__(self, data_dir):
        self.p = subprocess.Popen([BIN, "--data-dir", data_dir], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.n = 0
        first = self.p.stdout.readline().strip()
        assert json.loads(first) == {"type": "ready"}, first
    def q(self, sql, mode="exact"):
        self.n += 1
        req = {"id": self.n, "op": "execute", "sql": sql, "search_mode": mode}
        self.p.stdin.write(json.dumps(req) + "\n"); self.p.stdin.flush()
        line = self.p.stdout.readline()
        return json.loads(line)
    def close(self):
        try:
            self.n += 1
            self.p.stdin.write(json.dumps({"id": self.n, "op": "shutdown"}) + "\n"); self.p.stdin.flush()
            self.p.stdout.readline()
        except Exception:
            pass
        try: self.p.wait(timeout=10)
        except Exception: self.p.kill()

d = tempfile.mkdtemp()
s = S(d)
print(s.q("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(4), v VECTOR(2));"))
print(s.q("INSERT INTO t VALUES (1,'a','[1,1]');"))
print("multi-row with bad varchar:", s.q("INSERT INTO t VALUES (2,'b','[2,2]'), (3,'toolong','[3,3]');"))
print("after:", s.q("SELECT id FROM t ORDER BY id;"))
print("multi-row dup pk:", s.q("INSERT INTO t VALUES (4,'d','[4,4]'), (1,'e','[5,5]');"))
print("after:", s.q("SELECT id FROM t ORDER BY id;"))
print("bad vector in 2nd row:", s.q("INSERT INTO t VALUES (6,'f','[6,6]'), (7,'g','[nan,1]');"))
print("after:", s.q("SELECT id FROM t ORDER BY id;"))
print("update to too long:", s.q("UPDATE t SET s='toolong' WHERE id=1;"))
print("after:", s.q("SELECT id, s FROM t ORDER BY id;"))
s.close(); shutil.rmtree(d, ignore_errors=True)
