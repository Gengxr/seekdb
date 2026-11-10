# MiniVectorDB

2026 年 OceanBase 数据库大赛加赛 · 赛题一「完成一个轻量向量数据库」的实现。

单机、单进程、单客户端连接，通过标准输入接收 SQL，标准输出返回结果，持久化数据全部写在
`--data-dir`（默认 `/data`）。从零实现存储、SQL 解析与执行、向量检索、事务和崩溃恢复；
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

可选参数：`--cache-mb N`（内存中行缓存上限，默认 256）、`--protocol json|text`、
`--ready-marker STR`（默认不输出任何就绪标记）。无法识别的启动参数只写一行 stderr 警告、不退出，
避免评测驱动附加参数时整个用例失败。

## 2. 进程通信约定

| 方向 | 约定 |
| --- | --- |
| 输入 | stdin，UTF-8。语句以 `;` 结束，允许跨行；也接受一行一条不带分号的语句；如果驱动发来一条既没有 `;` 也没有换行的完整语句，输入空闲 200 ms 后也会执行；支持 `--`、`#`、`/* */` 注释 |
| 输出 | stdout，每条语句一行 JSON，写完立即写出（无缓冲） |
| 查询 | `{"columns":["id","content"],"rows":[[1,"初始"]]}` |
| 修改 | `{"affected_rows":3}` |
| DDL / 事务 / SET | `{}` |
| 错误 | `{"error":"duplicate_key"}`，语句继续执行 |
| 日志 | 全部写 stderr，不混入 stdout |
| 退出 | stdin 关闭、`QUIT`/`EXIT`、SIGTERM/SIGINT 都能正常退出 |

驱动指令 `--mode ann` / `--mode exact` 也接受（等价于 `SET vector_mode = ann|exact`）；
`--restart` 由测试驱动自己处理，程序看到时按幂等忽略。

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

```bash
$ rm -rf /tmp/d && ./minivectordb --data-dir /tmp/d
CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), embedding VECTOR(2));
{}
INSERT INTO docs VALUES (3,'乙','三','[0,2]'),(1,'甲','一','[0,0]'),(2,'甲','二','[1,0]');
{"affected_rows":3}
SELECT id, content FROM docs ORDER BY id;
{"columns":["id","content"],"rows":[[1,"一"],[2,"二"],[3,"三"]]}
BEGIN;
{}
UPDATE docs SET embedding = '[3,4]' WHERE id = 1;
{"affected_rows":1}
DELETE FROM docs WHERE id = 2;
{"affected_rows":1}
ROLLBACK;
{}
SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs ORDER BY distance LIMIT 10;
{"columns":["id","distance"],"rows":[[1,0.000000],[2,1.000000],[3,2.000000]]}
```

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

精确检索走全表扫描：向量在原行里的偏移在写入时算好（`vec_rel`），扫描时只取向量那一段，用 AVX2
双精度累加（`vcvtps2pd` + `vfmadd_pd`，无 AVX2 时退回标量）算平方距离，用"当前第 K 名距离"当阈值，
只有更小的候选才进入长度 K 的有序数组，最后再按 (距离, 主键) 排序。输出距离用 double 重新算一次平方根，
避免 float32 累加误差影响 1e-5 容差。

`CREATE VECTOR INDEX` 目前只记录元数据，`ann` 模式直接返回精确结果（Recall = 1.0）。题面允许查询在
提交配置里固定 exact 模式，这样召回率恒为 1，代价是扫描全表；实测 10,000 条 128 维向量上 1000 次
TopK 查询约 0.31 秒，瓶颈已经是内存带宽。

### 5.3 内存与大于内存的数据

每张表维护 `pk → slot` 的开地址哈希索引（约 13 字节/行）和 slot 数组（32 字节/行）。行字节默认缓存在
内存 arena 中（上限 `--cache-mb`，默认 256 MB），超过预算的表退化为按需 `pread`，用 4 MB 顺序读缓冲，
内存占用与数据量无关。开表时按 live 字节数从大到小决定哪些表能进缓存；写入过程中超出预算会淘汰最大的 arena。

### 5.4 事务与提交

未显式 `BEGIN` 时每条修改语句自成一个隐式事务。事务内的修改先进入内存 overlay（`pk → 编码后的行`，
删除记为墓碑），读自己写的修改时把 overlay 和已提交数据合并。语句失败时它自己的修改整体丢弃；
显式事务里之前成功的语句仍在 overlay 里，可以继续提交或回滚。

`COMMIT` 把一个事务的全部操作序列化成一条记录追加到 `mvd.log`，`fdatasync` 落盘后才回复成功，
然后才更新内存结构。因此"已确认提交"的数据一定在磁盘上。

### 5.5 恢复

启动时按顺序扫描 `mvd.log`：magic 不符、长度越界或 CRC 不符即认为尾部是崩溃留下的半个记录，把文件
截断到最后一条完整记录，再重建 `pk → slot` 索引。重放过程不修改日志内容，所以在恢复途中再次被强杀，
下次启动照样从头重放，结果一致。索引重建完成前不输出任何就绪标记。

日志膨胀到 `live × 4 + 8 MB` 以上且超过 64 MB 时会把存活行重写到新日志（临时文件 + `fdatasync` +
`rename`），保证恢复时间和磁盘占用有界。

## 6. 已实现功能与已知限制

已实现：题面 2.1 表格里的全部能力（建表、增删改查、条件查询、精确 TopK、带过滤 TopK、持久化、
事务、崩溃恢复），以及多表、`VARCHAR` 码点长度与码点序比较、非法向量拒绝、大于内存数据、日志压缩。

已知限制：

- 同一时刻只支持一个连接和一个显式事务（题面允许）。
- `CREATE VECTOR INDEX` 只记录元数据，`ann` 模式实际仍走精确扫描（召回率 1.0，但没有做近似加速）。
- 不支持 `NULL`、`JOIN`、聚合、子查询、`ALTER TABLE`、外键、主键更新、`OFFSET`（题面不要求）。
- `UPDATE`/`DELETE` 走全表扫描，没有二级索引；单表行数很大时这两类语句会明显变慢。
- 日志记录按批次写，单事务修改量特别大时会有一次较大的内存拷贝。

## 7. 本地验证记录

`minivectordb/tests/` 下是三个本地测试脚本，运行记录如下（服务器 24 核 / 60 GB，数据目录在 /data）：

| 测试 | 内容 | 结果 |
| --- | --- | --- |
| `run_public_cases.py` | 题包 20 个公开用例、95 条 SQL | 20/20 通过 |
| `crash_test.py` | SIGKILL 后已提交数据保留、未提交事务不生效、多行插入原子性、恢复途中再次强杀 | 通过 |
| `large_data.py` | 120,000 行 × 128 维，缓存预算 8 MB（数据 65.6 MB） | 写入 5.54 s，重启后向量 Top5 8.9 ms，峰值 RSS 79 MB |

性能自测（10,000 行 × 128 维，100 条一批写入并提交，1000 次 TopK K=10，一半带等值过滤）：

| 指标 | 数值 |
| --- | --- |
| 批量写入 | 0.52 s |
| 1000 次向量查询 | 0.31 s（约 3200 QPS） |
| 峰值内存 | 21 MB |
| 数据目录大小 | 5.6 MB |

公开用例的 .test/.result 文件来自官方 `2026-competition-round2` 分支的 `mysqltest/`；本地驱动按题面
约定逐条发送 SQL 并按 `1e-5` 容差比较 JSON 断言。测试脚本只用于自测，不参与正式计分。
