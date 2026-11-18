"""Run against a built MiniVectorDB: python3 test_topk_regressions.py BINARY."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    with tempfile.TemporaryDirectory() as data:
        binary = str(Path(sys.argv[1]).resolve())
        with subprocess.Popen([binary, "--data-dir", data], text=True,
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE) as proc:
            assert json.loads(proc.stdout.readline()) == {"type": "ready"}

            def execute(sql):
                proc.stdin.write(json.dumps({"id": 1, "op": "execute", "sql": sql}) + "\n")
                proc.stdin.flush()
                answer = json.loads(proc.stdout.readline())
                assert answer["ok"], answer
                return answer.get("rows")

            execute("CREATE TABLE t(id BIGINT PRIMARY KEY, grp BIGINT, v VECTOR(1))")
            execute("INSERT INTO t VALUES(-2,1,'[2e-37]'),(7,1,'[1e-37]')")
            ordered = "SELECT id FROM t WHERE grp=1 ORDER BY L2_DISTANCE(v,'[0]') LIMIT 10"
            assert execute(ordered) == [[7], [-2]]
            execute("UPDATE t SET v='[3e-37]' WHERE id=-2")
            assert execute(ordered) == [[7], [-2]]
            execute("BEGIN")
            execute("UPDATE t SET v='[0]' WHERE id=7")
            assert execute(ordered) == [[7], [-2]]
            execute("ROLLBACK")
            execute("DELETE FROM t WHERE id=7")
            execute("INSERT INTO t VALUES(7,1,'[3e-37]')")
            assert execute(ordered) == [[-2], [7]]
            proc.stdin.write('{"id":2,"op":"shutdown"}\n')
            proc.stdin.flush()
            assert json.loads(proc.stdout.readline()) == {"id": 2, "ok": True}
            assert proc.wait(timeout=10) == 0


if __name__ == "__main__":
    main()
