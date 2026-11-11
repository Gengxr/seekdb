import json, math, subprocess, tempfile, shutil, sys
BIN = sys.argv[1] if len(sys.argv) > 1 else "/usr/local/bin/minivectordb"

class S:
    def __init__(self, d):
        self.p = subprocess.Popen([BIN, "--data-dir", d], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.n = 0
        assert json.loads(self.p.stdout.readline()) == {"type": "ready"}
    def q(self, sql):
        self.n += 1
        self.p.stdin.write(json.dumps({"id": self.n, "op": "execute", "sql": sql, "search_mode": "exact"}) + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())
    def close(self):
        self.n += 1
        self.p.stdin.write(json.dumps({"id": self.n, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        self.p.wait(timeout=10)

fails = []
def check(label, cond, got=None):
    print(("PASS " if cond else "FAIL ") + label + ("" if cond else "  got=" + json.dumps(got, ensure_ascii=False)))
    if not cond: fails.append(label)

d = tempfile.mkdtemp()
s = S(d)
s.q("CREATE TABLE pts (id BIGINT PRIMARY KEY, v VECTOR(2));")
# four points at distance 1 from origin: ties must order by pk asc
for i, v in [(3, "[1,0]"), (1, "[-1,0]"), (4, "[0,-1]"), (2, "[0,1]")]:
    s.q("INSERT INTO pts VALUES (%d, '%s');" % (i, v))
r = s.q("SELECT id, L2_DISTANCE(v,'[0,0]') AS d FROM pts ORDER BY d LIMIT 4;")
check("ties order by pk asc", r["rows"] == [[1,1.0],[2,1.0],[3,1.0],[4,1.0]], r)
check("distance value", abs(r["rows"][0][1] - 1.0) < 1e-9, r)

# string ordering by code point
s.q("CREATE TABLE st (id BIGINT PRIMARY KEY, s VARCHAR(10));")
for i, v in [(1, "b"), (2, "A"), (3, "中"), (4, "a"), (5, "Z")]:
    s.q("INSERT INTO st VALUES (%d, '%s');" % (i, v))
r = s.q("SELECT s FROM st ORDER BY s;")
check("varchar codepoint order", [x[0] for x in r["rows"]] == ["A","Z","a","b","中"], r)

# where on string
r = s.q("SELECT id FROM st WHERE s >= 'a' ORDER BY id;")
check("varchar where", [x[0] for x in r["rows"]] == [1,3,4], r)

# limit larger than result / limit 0
r = s.q("SELECT id FROM st ORDER BY id LIMIT 100;")
check("limit beyond rows", len(r["rows"]) == 5, r)
r = s.q("SELECT id FROM st ORDER BY id LIMIT 0;")
check("limit zero", r["rows"] == [] and r["columns"] == ["id"], r)

# delete / update without where
r = s.q("UPDATE st SET s = 'z';")
check("update all rows", r.get("affected_rows") == 5, r)
r = s.q("DELETE FROM st WHERE id > 3;")
check("delete filtered", r.get("affected_rows") == 2, r)
r = s.q("DELETE FROM st;")
check("delete all", r.get("affected_rows") == 3, r)

# duplicate key rejected, row count unchanged
r = s.q("INSERT INTO pts VALUES (1, '[9,9]');")
check("duplicate pk", r.get("error", {}).get("code") == "duplicate_key", r)

# float precision of non-trivial distance
s.q("CREATE TABLE f (id BIGINT PRIMARY KEY, v VECTOR(3));")
s.q("INSERT INTO f VALUES (1, '[1,2,3]');")
r = s.q("SELECT L2_DISTANCE(v,'[0,0,0]') AS d FROM f;")
check("sqrt(14) precision", abs(r["rows"][0][0] - math.sqrt(14)) < 1e-9, r)

# negative components and -0.0
s.q("INSERT INTO f VALUES (2, '[-1,-2,-3]');")
r = s.q("SELECT id, L2_DISTANCE(v,'[0,0,0]') AS d FROM f ORDER BY d;")
check("negative components", r["rows"][0][1] == r["rows"][1][1], r)

# two vector columns rejected
r = s.q("CREATE TABLE bad (id BIGINT PRIMARY KEY, a VECTOR(2), b VECTOR(2));")
check("single vector column", not r.get("ok", True), r)

# vector index on a text column rejected
r = s.q("CREATE VECTOR INDEX ix ON st(s);")
check("index on non-vector", not r.get("ok", True), r)

# select * shape on vector table
r = s.q("SELECT * FROM f ORDER BY id;")
check("select star vector", r["columns"] == ["id","v"] and r["rows"][0][1] == [1.0,2.0,3.0], r)

# empty table query still returns columns
s.q("CREATE TABLE empty_t (id BIGINT PRIMARY KEY, v VECTOR(2));")
r = s.q("SELECT id, v FROM empty_t;")
check("empty table columns", r["columns"] == ["id","v"] and r["rows"] == [], r)

# transaction: rollback hides writes, commit persists
s.q("BEGIN;"); s.q("INSERT INTO empty_t VALUES (7, '[1,1]');")
r = s.q("SELECT id FROM empty_t;")
check("read own write", r["rows"] == [[7]], r)
s.q("ROLLBACK;")
r = s.q("SELECT id FROM empty_t;")
check("rollback hides", r["rows"] == [], r)
s.close()

s2 = S(d)
r = s2.q("SELECT id FROM pts ORDER BY id;")
check("restart keeps committed", [x[0] for x in r["rows"]] == [1,2,3,4], r)
s2.close()
shutil.rmtree(d, ignore_errors=True)
print("RESULT", "ALL PASS" if not fails else ("FAILURES: " + ", ".join(fails)))
