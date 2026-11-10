# 本地测试脚本

判题协议是官方 `STDIO_PROTOCOL.md` 的 JSON Lines（stdin 一行一条 JSON 请求，stdout 一行一条 JSON 响应）。
所有当前脚本都按这个协议和进程对话。

| 脚本 | 用途 | 状态 |
| --- | --- | --- |
| `crash_test.py` | SIGKILL 后已提交保留、未提交不残留、多行插入原子性、恢复途中再次强杀 | 当前协议，通过 |
| `large_data.py` | 120,000 行 × 128 维、缓存预算 8 MB 的超内存数据与重启后查询 | 当前协议，通过 |
| `json_probe.py` | 握手、ann/exact、`ORDER BY L2_DISTANCE(...)`、`SELECT *`、非法向量、空结果、重启持久化 | 当前协议，通过 |
| `differential_test.py` | 与独立 Python 参考模型做随机对照（60 步 × 8 seed） | 旧文本协议，语义部分仍可参考 |
| `bench.py` | 10,000 行 × 128 维写入 / TopK 吞吐 | 旧文本协议 |
| `run_public_cases.py` | 早期自写驱动跑公开用例 | 已被官方 `protocol_check` 取代 |
| `repro_case4.py` / `silent_probe.py` / `shape_probe.py` / `fuzz_test.py` | 协议未公开时期的临时探测脚本 | 历史存档，不再使用 |

正式的自测入口是官方仓库的判题器：

```bash
python3 protocol_check/run.py --command '["/path/to/minivectordb","--data-dir","{data_dir}"]' --output /tmp/pc-out
python3 protocol_check/run.py --image minivectordb:proto --output /tmp/pc-img-out
```
