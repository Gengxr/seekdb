# MiniVectorDB

2026 年 OceanBase 数据库大赛加赛 · 赛题一「完成一个轻量向量数据库」的实现。

单机、单进程、单客户端连接，通过标准输入接收一行一条 JSON 请求，标准输出返回一行一条 JSON 响应
（官方 `STDIO_PROTOCOL.md` 的 JSON Lines 约定），持久化数据全部写在 `--data-dir`（默认 `/data`）。
从零实现存储、SQL 解析与执行、向量检索、事务和崩溃恢复；
没有使用 SQLite、RocksDB、Faiss、VSAG、HNSWlib 等现成引擎或索引库，第三方依赖只有 libc 和 libstdc++。

## 1. 语言、构建与启动

| 项目 | 值 |
| --- | --- |
| 语言 | C++20（`-std=c++20`） |
| 编译器 | GCC 13（Ubuntu 24.04） |
| 构建 | CMake ≥ 3.16，Release 默认 `-O3` |
| 运行环境 | Linux x86_64 |
| 依赖 | 无（libstdc++/libgcc 静态链入） |

镜像构建：

```bash
docker build -t minivectordb:latest .
```

镜像为两阶段构建：第一阶段用 `ubuntu:24.04` 安装 `g++`/`cmake` 并从源码编译，第二阶段只拷贝
二进制。运行入口固定为 `/usr/local/bin/minivectordb --data-dir /data`。

前台运行（`-i` 保持 stdin，不加 `-t`/`-d`，不暴露端口）：

```bash
docker run --rm -i -v minivectordb-data:/data --cpus=2 --memory=2g minivectordb:latest
```

本地不经过容器直接运行：

```bash
cmake -S minivectordb -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/minivectordb --data-dir /tmp/mvd
```

可选参数：`--cache-mb N`（内存中行缓存上限，默认 256）。数据目录和缓存预算也可以分别用环境变量
`VECTORDB_DATA_DIR`、`MVD_CACHE_MB` 给出。无法识别的启动参数只写一行 stderr 警告、不退出，
避免评测驱动附加参数时整个用例失败。

## 2. 进程通信约定（JSON Lines）

按官方 `STDIO_PROTOCOL.md`（`2026-competition-round2` 分支）实现：

| 方向 | 约定 |
| --- | --- |
| 输入 | stdin，一行一条 UTF-8 JSON 请求：`{"id":1,"op":"execute","sql":"SELECT ...","search_mode":"exact"}` |
| 输出 | stdout，一行一条 JSON 响应，写完立即 flush；stdout 只放协议报文，诊断日志全部走 stderr |
| 启动 | 完成恢复、开始读请求之前输出 `{"type":"ready"}`，每次启动（含重启）都输出一次 |
| 查询 | `{"id":1,"ok":true,"columns":["id"],"rows":[[1],[2]]}`；BIGINT 是 JSON 整数，VECTOR 是 JSON 数组，空结果也返回列名 |
| 修改 | `{"id":2,"ok":true,"affected_rows":1}` |
| 其他语句 | `{"id":3,"ok":true}`，`ok` 必须是 JSON 布尔值 |
| 错误 | `{"id":4,"ok":false,"error":{"code":"duplicate_key","message":"..."}}`，出错后会话继续 |
| 关闭 | `{"id":5,"op":"shutdown"}`：回滚未提交事务，回 `{"id":5,"ok":true}`，退出码 0，之后不再输出任何字节 |

请求里的 `id` 原样回传（数字仍是数字、字符串仍是字符串）；`search_mode` 为 `ann` 时按 `SET
vector_mode = ann` 处理，`exact` 时按精确模式处理。`sql` 里的语句可以带结尾分号，程序不做分号切分。

## 3. SQL 支持范围

```sql
CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), content VARCHAR(1024),
                   embedding VECTOR(4));
CREATE TABLE tags (id BIGINT, tag VARCHAR(8), PRIMARY KEY (id));
CREATE VECTOR INDEX docs_vec ON docs(embedding);

INSERT INTO docs VALUES (1,'database','事务与恢复','[1,0,0,0]'), (2,'database','向量索引','[0,1,0,0]');
INSERT INTO docs (id, category) VALUES (3,'travel');
SELECT id, content, L2_DISTANCE(embedding, '[0.9,0.1,0,0]') AS distance
  FROM docs WHERE category = 'database' ORDER BY distance LIMIT 2;
UPDATE docs SET embedding = '[0.8,0.2,0,0]' WHERE id = 1;
DELETE FROM docs WHERE id = 2;
BEGIN; ... COMMIT; / ROLLBACK;
SET vector_mode = ann;   -- 或 exact，默认 exact
```

语义：

- 类型 `BIGINT`、`VARCHAR(n)`（长度按 Unicode 码点计）、`VECTOR(n)`（float32）；一个 BIGINT 主键，
  每表最多一个向量列。主键不可重复，不支持主键更新。
- 向量元素必须是可表示为有限 float32 的数值；NaN、Infinity、维度不符、float32 溢出都拒绝。
- 距离按已存储的 float32 向量计算，`L2_DISTANCE` 返回欧氏距离（不是平方距离），累加用 double。
- 过滤后的 TopK 先应用 WHERE 再算距离，不会先取全表 K 条再过滤；不足 K 条返回全部。
- 距离并列按主键升序；普通 `ORDER BY` 并列也按主键升序；没有 `ORDER BY` 时按主键升序输出。
- 未显式 `BEGIN` 时，每条成功的修改语句自动提交。
- 不支持的语法（NULL、JOIN、聚合、子查询、ALTER TABLE、外键、嵌套事务、OR/IN/LIKE 等）
  返回 `unsupported_sql`，不会静默执行成别的含义。

错误码：`duplicate_key`、`unsupported_sql`、`syntax_error`、`dimension_mismatch`、
`invalid_vector`、`invalid_value`、`string_too_long`、`integer_overflow`、`table_not_found`、
`table_exists`、`column_not_found`、`column_exists`、`index_exists`、`primary_key_missing`、
`transaction_active`、`no_transaction`、`io_error`、`internal_error`。

## 4. 从空目录开始的完整示例

启动后先输出 `{"type":"ready"}`，之后每条请求一行：

| # | 请求里的 `sql` | 响应 |
| --- | --- | --- |
| 1 | `CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), embedding VECTOR(2));` | `{"id":1,"ok":true}` |
| 2 | `INSERT INTO docs VALUES (3,'乙','三','[0,2]'),(1,'甲','一','[0,0]'),(2,'甲','二','[1,0]');` | `{"id":2,"ok":true,"affected_rows":3}` |
| 3 | `SELECT id, content FROM docs ORDER BY id;` | `{"id":3,"ok":true,"columns":["id","content"],"rows":[[1,"一"],[2,"二"],[3,"三"]]}` |
| 4 | `BEGIN;` | `{"id":4,"ok":true}` |
| 5 | `UPDATE docs SET embedding = '[3,4]' WHERE id = 1;` | `{"id":5,"ok":true,"affected_rows":1}` |
| 6 | `DELETE FROM docs WHERE id = 2;` | `{"id":6,"ok":true,"affected_rows":1}` |
| 7 | `ROLLBACK;` | `{"id":7,"ok":true}` |
| 8 | `SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs ORDER BY distance LIMIT 10;` | `{"id":8,"ok":true,"columns":["id","distance"],"rows":[[1,0],[2,1],[3,2]]}` |

回滚之后记录内容和向量检索都回到事务前状态：事务里被改掉的 `id=1` 向量、被删掉的 `id=2` 记录都恢复。
重启（重新执行最后一条 SELECT）得到同样的输出，说明正常关闭时的落盘和启动时的恢复一致。

## 5. 设计与实现

### 5.1 记录布局

数据目录：

| 文件 | 内容 |
| --- | --- |
| `catalog.mvd` | 表定义、列类型、主键、是否建了向量索引、表 id 分配；临时文件 + `fdatasync` + `rename` 原子替换 |
| `mvd.log` | 追加写的提交日志，既是 redo 也是数据文件 |
| `LOCK` | `flock` 独占锁，防止两个进程同时打开同一数据目录 |

`mvd.log` 每条记录是一个提交批次：24 字节头（magic、版本、类型、payload 长度、payload CRC32）+ payload。
payload 是 `nops` 加若干操作，每个操作为 `opcode(1) table_id(4) pk(8) rowlen(4) vec_rel(4) 行字节`。
行字节按 schema 顺序编码：BIGINT 8 字节、VARCHAR 4 字节长度 + UTF-8 字节、VECTOR n×4 字节 float32。

### 5.2 向量检索

对于常驻内存的向量表，执行器把一组向量按 32 行分块，每 4 个维度连续排列。查询先用整数点积求量化距离，再用数据和查询的量化残差上界确定候选范围。每个保留的候选都用原向量计算双精度 L2；并列按主键升序。整数内核支持 AVX2，可用时采用 AVX-VNNI。维度、内存、指令集或 K 超出适用范围时，执行器使用精确扫描。

等值过滤先取得匹配行。主键等值与距离排序组合时直接定位该行。反复查询同一低基数列时，执行器可在内存预算内将同类行的整数编码连续排列。过滤查询只扫描对应分类的块，无过滤查询仍使用原有行序。匹配行很少时直接读取原向量。

写入成功后，若行仍能放进现有量化网格，执行器更新对应行的编码和误差上界。删除会将该行标为空位。数值超出网格或需要增加分块时，在后续检索中重新构建索引。分类副本在写入后失效。显式事务合并未提交的行。`CREATE VECTOR INDEX` 记录索引元数据，`ann` 请求也走精确检索。

### 5.3 内存与大于内存的数据

行内容可缓存在内存中，超出 `--cache-mb` 预算的大表按需从日志读取。主键和行偏移仍占用随行数增长的内存。按照主键等值查行时先检查事务内的更改，再查开放寻址哈希表。启动时合并读取相邻的日志记录，单个超过读窗口的事务按记录长度读取。

### 5.4 事务与提交

未显式 `BEGIN` 时每条修改语句自成一个隐式事务。事务内的修改先进入内存 overlay（`pk → 编码后的行`，
删除记为墓碑），读自己写的修改时把 overlay 和已提交数据合并。语句失败时它自己的修改整体丢弃；
显式事务里之前成功的语句仍在 overlay 里，可以继续提交或回滚。

`COMMIT` 把一个事务的全部操作序列化成一条记录追加到 `mvd.log`，`fdatasync` 落盘后才回复成功，
然后才更新内存结构。因此"已确认提交"的数据一定在磁盘上。

### 5.5 恢复

启动时按顺序扫描 `mvd.log`：magic 不符、长度越界或 CRC 不符即认为尾部是崩溃留下的半个记录，把文件
截断到最后一条完整记录，再重建 `pk → slot` 索引。重放过程不修改日志内容，所以在恢复途中再次被强杀，
下次启动照样从头重放，结果一致。恢复完成后才输出 `{"type":"ready"}`，所以驱动看到 ready 时数据已经可用。

日志膨胀到 `live × 4 + 8 MB` 以上且超过 64 MB 时会把存活行重写到新日志（临时文件 + `fdatasync` +
`rename`），保证恢复时间和磁盘占用有界。

## 6. 已实现功能与已知限制

已实现：题面 2.1 表格里的全部能力（建表、增删改查、条件查询、精确 TopK、带过滤 TopK、持久化、
事务、崩溃恢复），以及多表、`VARCHAR` 码点长度与码点序比较、非法向量拒绝、大于内存数据、日志压缩。

已知限制：

- 同一时刻只支持一个连接和一个显式事务（题面允许）。
- `CREATE VECTOR INDEX` 只记录元数据，`ann` 模式实际仍走精确扫描（召回率 1.0，但没有做近似加速）。
- 不支持 `NULL`、`JOIN`、聚合、子查询、`ALTER TABLE`、外键、主键更新、`OFFSET`（题面不要求）。
- `UPDATE` 和 `DELETE` 的主键等值条件使用哈希索引，其他更新和删除条件需要扫描可见记录。
- 日志记录按批次写，单事务修改量特别大时会有一次较大的内存拷贝。

## 7. 本地验证记录

11 月 18 日在同一 Linux 服务器上做版本对照。每组含 10000 条 128 维向量和 1000 条 Top10 查询，其中一半带等值过滤，用三个随机种子各跑两轮。驱动预先编码请求，测量发送请求到收齐应答的时间，再逐条与独立的双精度全量排序核对。4、20、100 类时，本版耗时中位数分别为 21.92、20.52、18.77 毫秒，前一版为 25.25、22.89、19.14 毫秒。低基数分类副本会额外占用内存，空间不足时使用直接扫描。

日志保持原有的 IEEE CRC32 格式。新旧版本对同一套事务写出的 25 条记录逐字节一致，载荷长度覆盖除以 8 的全部余数，逐条通过独立 CRC 实现复核。两版互读由另一版写出的日志，均能恢复。截断最后一条事务或改动其载荷后，恢复只保留之前的完整事务，之后写入的记录重启仍可读。

以下为较早版本的验证记录。

官方仓库自带的判题器 `protocol_check` 两种模式都跑过（服务器 24 核 / 60 GB，数据目录在 /data）：

| 测试 | 内容 | 结果 |
| --- | --- | --- |
| `protocol_check --command` | 直接跑本地二进制，20 个公开用例 | 20/20 通过，4~10 ms/用例 |
| `protocol_check --image` | 用评测参数跑镜像（`--read-only`、无网络、2 CPU、2 GiB、128 PID、cap-drop） | 20/20 通过，138~292 ms/用例 |
| `tests/json_probe.py` | 握手、`ann`/`exact` 切换、`ORDER BY L2_DISTANCE(...)`、`SELECT *`、非法向量、空结果、重启持久化 | 全部符合协议 |

存储层的语义测试（SIGKILL 恢复、120,000 行 × 128 维超缓存数据、随机对照 8 组 seed）在协议改造前用
文本协议跑过，存储与执行部分未改动；协议层改造后用 `json_probe.py` 复验了重启持久化与事务关闭语义。

性能自测（10,000 行 × 128 维，100 条一批写入并提交，1000 次 TopK K=10，一半带等值过滤）：

| 指标 | 数值 |
| --- | --- |
| 批量写入 | 0.52 s |
| 1000 次向量查询 | 0.31 s（约 3200 QPS） |
| 峰值内存 | 21 MB |
| 数据目录大小 | 5.6 MB |

公开用例的 .test/.result 文件来自官方 `2026-competition-round2` 分支的 `mysqltest/`；判题协议以同分支
`STDIO_PROTOCOL.md` 和 `protocol_check/` 为准。测试脚本只用于自测，不参与正式计分。
