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
            # Reordered equality postings must refer to the same rows as the
            # unfiltered scan and an independent equality column after updates.
            execute("CREATE TABLE grouped(id BIGINT PRIMARY KEY, bucket BIGINT, name VARCHAR(6), v VECTOR(8))")
            values = {}
            for offset in range(0, 1024, 64):
                batch = []
                for i in range(offset, offset + 64):
                    values[i] = (i % 4, f"b{i % 3}", i % 11)
                    batch.append(f"({i},{i % 4},'b{i % 3}','[{i % 11},0,0,0,0,0,0,0]')")
                execute("INSERT INTO grouped VALUES " + ",".join(batch))

            def check_group(predicate, accept):
                expected = sorted(((x[2] - 4) ** 2, pk) for pk, x in values.items() if accept(x))
                rows = execute("SELECT id FROM grouped " + predicate +
                               " ORDER BY L2_DISTANCE(v,'[4,0,0,0,0,0,0,0]') LIMIT 32")
                assert rows == [[pk] for _, pk in expected[:32]]

            for tick in range(3):
                for group in (1, 2, 0, 3):
                    check_group(f"WHERE bucket={group}", lambda x: x[0] == group)
                check_group("", lambda x: True)
                check_group("WHERE name='b2'", lambda x: x[1] == "b2")
                key = 4 + tick
                execute(f"DELETE FROM grouped WHERE id={key}")
                del values[key]
                check_group("WHERE bucket=0", lambda x: x[0] == 0)
                execute(f"INSERT INTO grouped VALUES({key},2,'b2','[4,0,0,0,0,0,0,0]')")
                values[key] = (2, "b2", 4)
            check_group("WHERE bucket=2", lambda x: x[0] == 2)
            proc.stdin.write('{"id":2,"op":"shutdown"}\n')
            proc.stdin.flush()
            assert json.loads(proc.stdout.readline()) == {"id": 2, "ok": True}
            assert proc.wait(timeout=10) == 0


if __name__ == "__main__":
    main()
