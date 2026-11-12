import json, subprocess, tempfile, shutil, sys
BIN = "/data/henchi/projects/oceanbase_minivectordb_20261109/work/minivectordb/build/minivectordb"
class S:
    def __init__(self, d):
        self.p = subprocess.Popen([BIN, "--data-dir", d], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.n = 0
        assert json.loads(self.p.stdout.readline()) == {"type": "ready"}
    def q(self, sql):
        self.n += 1
        self.p.stdin.write(json.dumps({"id": self.n, "op": "execute", "sql": sql, "search_mode": "exact"}) + "\n"); self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())
    def close(self):
        self.n += 1
        self.p.stdin.write(json.dumps({"id": self.n, "op": "shutdown"}) + "\n"); self.p.stdin.flush(); self.p.wait(timeout=10)

d = tempfile.mkdtemp(); s = S(d)
print(s.q("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(40));"))
vals = ["O''Brien", "back\\slash", "tab\tchar", "quote\"double", "", "空格 中文"]
for i, v in enumerate(vals, 1):
    lit = v.replace("'", "''")
    r = s.q("INSERT INTO t VALUES (%d, '%s');" % (i, lit))
    print(i, r)
r = s.q("SELECT id, s FROM t ORDER BY id;")
for row in r["rows"]:
    print(row[0], repr(row[1]), row[1] == vals[row[0]-1])
r2 = s.q("SELECT id FROM t WHERE s = 'O''Brien';")
print("where quote", r2)
s.close(); shutil.rmtree(d, ignore_errors=True)
