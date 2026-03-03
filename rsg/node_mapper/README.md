# node_mapper — MySQL 语法节点到 SQL 命令映射发现工具

## 概述

`node_mapper` 是一个自动化工具，用于发现 MySQL 语法文件（`.y`）中的 AST
节点（grammar rule）与 MySQL 内部枚举类型 `enum_sql_command` 之间的对应关系。

工具的核心问题是：**给定 MySQL 语法中的一个规则名（如 `insert_stmt`），
当这条规则生成的 SQL 被执行时，MySQL 服务器内部会使用哪个 `SQLCOM_*` 枚举值？**

这一映射关系可用于：

- 引导模糊测试器（fuzzer）定向生成特定 SQL 命令类型
- 分析语法规则的覆盖情况
- 辅助 MySQL 源码审计

---

## 工作原理

```
语法文件 (mysql_sql.y)
        │
        ▼
  [1] 解析语法，枚举 simple_statement 的所有直接子规则（135 个）
        │
        ▼
  [2] SimpleRSG 对每个规则随机生成 N 条 SQL（默认 N=200）
        │  写入 queries.tsv（格式：node<TAB>sql）
        ▼
  [3] C++ executor 逐条将 SQL 发往运行中的 MySQL 8.0.33
        │  MySQL 服务器在执行每条语句时向 stderr 输出：
        │    SQLCOM_EXEC: <整数>
        │  executor 轮询 /tmp/mysql_stderr.log 捕获该整数
        │  输出：node<TAB>sqlcom_int<TAB>status
        ▼
  [4] Go 端汇总统计，输出 node_sqlcom_mapping.json
```

### MySQL 服务器的 patch

patched mysqld 在 `sql/sql_parse.cc:5327` 增加了以下一行：

```cpp
fprintf(stderr, "SQLCOM_EXEC: %d\n", thd->lex->sql_command);
```

该行在每次 `mysql_execute_command` 执行时将 `enum_sql_command` 的整数值写入
mysqld 的 **stderr**（需将其重定向到日志文件）。

---

## 目录结构

```
rsg/node_mapper/
├── main.go                   # Go 主程序：语法解析、SQL 生成、调度、JSON 输出
├── simple_rsg.go             # SimpleRSG：轻量级随机 SQL 生成器（无 MAB/反馈）
├── Makefile                  # 构建与运行入口
├── executor/
│   └── main.cpp              # C++ 执行器：连接 MySQL、捕获 SQLCOM_EXEC
├── node_sqlcom_mapping.json  # 最新一次运行的输出（122/135 节点已映射）
└── queries.tsv               # 最新一次运行生成的全部查询（TSV 格式）
```

---

## 前置条件

| 依赖 | 版本要求 |
|------|----------|
| Go | 1.17+ |
| g++ | 支持 C++17 |
| libmysqlclient-dev | 提供 `/usr/include/mysql` 和 `libmysqlclient` |
| patched mysqld | MySQL 8.0.33（含 `SQLCOM_EXEC` patch） |

确认 patched mysqld 的位置：

```
mysql-server-mysql-8.0.33/bld/runtime_output_directory/mysqld
```

---

## 环境准备：启动 MySQL

工具需要一个**已运行的 patched MySQL 实例**，且其 stderr 被重定向到日志文件。
项目提供了 `setup_mysql.sh` 辅助脚本，或可手动启动：

```bash
# 1. 初始化数据目录（只需执行一次）
mysqld --initialize-insecure \
       --datadir=/tmp/mysql_node_mapper_data \
       --user=$(whoami)

# 2. 启动 mysqld，将 stderr 重定向到日志文件
mysqld \
  --datadir=/tmp/mysql_node_mapper_data \
  --pid-file=/tmp/mysql_node_mapper.pid  \
  --port=3306                            \
  --socket=/tmp/mysql_node_mapper.sock   \
  --daemonize                            \
  2>>/tmp/mysql_stderr.log
```
// --skip-grant-table can't with port
> **注意**：`2>>/tmp/mysql_stderr.log` 是关键，工具通过轮询该文件读取
> `SQLCOM_EXEC` 输出。若文件路径不同，需通过 `-log-file` 参数指定。

验证 MySQL 是否正常运行：

```bash
mysql -u root -h 127.0.0.1 -P 3306 -e "SELECT 1"
```

---

## 构建

```bash
# 在 rsg/node_mapper/ 目录下执行
make all       # 同时构建 Go 二进制和 C++ 执行器
make go        # 仅构建 Go 二进制 (node_mapper)
make cpp       # 仅构建 C++ 执行器 (executor/executor)
make clean     # 删除构建产物
```

---

## 使用方法

### 方式一：使用 Makefile（推荐）

```bash
make run
```

可通过变量覆盖默认参数：

```bash
make run N=200 DEPTH=2 LOG_FILE=/tmp/mysql_stderr.log OUTPUT=./my_mapping.json
```

所有可覆盖的 Makefile 变量：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `N` | `200` | 每个节点生成的 SQL 数量 |
| `DEPTH` | `2` | RSG 递归生成深度 |
| `LOG_FILE` | `/tmp/mysql_stderr.log` | mysqld stderr 日志路径 |
| `GRAMMAR` | `../parser_def_files/mysql_sql.y` | MySQL 语法文件路径 |
| `OUTPUT` | `./node_sqlcom_mapping.json` | 输出 JSON 路径 |
| `MYSQL_HOST` | `127.0.0.1` | MySQL 主机 |
| `MYSQL_PORT` | `3306` | MySQL 端口 |
| `MYSQL_USER` | `root` | MySQL 用户名 |
| `MYSQL_PASS` | _(空)_ | MySQL 密码 |
| `MYSQL_DB` | `node_mapper_db` | 工作数据库名（运行期间自动创建/删除） |

### 方式二：直接调用 Go 二进制

```bash
./node_mapper [flags]
```

完整参数列表：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-grammar` | `../parser_def_files/mysql_sql.y` | MySQL `.y` 语法文件路径 |
| `-executor` | `./executor/executor` | C++ 执行器二进制路径 |
| `-log-file` | `/tmp/mysql_stderr.log` | mysqld stderr 日志文件路径 |
| `-output` | `./node_sqlcom_mapping.json` | 输出 JSON 文件路径 |
| `-queries-file` | `./queries.tsv` | 中间 TSV 查询文件路径 |
| `-n` | `200` | 每个节点生成的 SQL 查询数量 |
| `-depth` | `2` | RSG 生成深度 |
| `-seed` | `0`（使用系统时间） | 随机种子（0 表示时间戳） |
| `-extra-roots` | _(空)_ | 额外探测的根节点，逗号分隔 |
| `-host` | `127.0.0.1` | MySQL 主机 |
| `-port` | `3306` | MySQL 端口 |
| `-user` | `root` | MySQL 用户名 |
| `-password` | _(空)_ | MySQL 密码 |
| `-database` | `node_mapper_db` | 工作数据库名 |
| `-poll-timeout-ms` | `100` | 等待 `SQLCOM_EXEC` 输出的最大毫秒数 |

示例：以固定种子运行，每节点生成 50 条 SQL：

```bash
./node_mapper -n 50 -seed 12345 -output ./mapping_n50.json
```

探测额外的根节点（不在 `simple_statement` 直接子规则中的节点）：

```bash
./node_mapper -extra-roots "begin_stmt,simple_statement"
```

---

## 输出文件说明

### `queries.tsv`

每行一条查询，格式为 `节点名<TAB>SQL语句`，例如：

```
insert_stmt	INSERT INTO t ( a ) VALUES ( 1 )
select_stmt	SELECT 1
alter_table_stmt	ALTER TABLE t ADD COLUMN c INT
```

### `node_sqlcom_mapping.json`

顶层结构：

```json
{
  "generated_at": "2026-03-03T16:19:44+08:00",
  "grammar_file": "../parser_def_files/mysql_sql.y",
  "queries_per_node": 50,
  "mappings": [ ... ],
  "sqlcom_to_nodes": { ... }
}
```

#### `mappings` 数组

每个元素对应一个语法节点：

```json
{
  "node": "insert_stmt",
  "sqlcom_values": [5],
  "sqlcom_names":  ["SQLCOM_INSERT"],
  "dominant_sqlcom": 5,
  "dominant_sqlcom_name": "SQLCOM_INSERT",
  "stats": {
    "sqlcom_freq": { "5": 48 },
    "status_freq": { "normal": 48, "semantic_error": 2 },
    "total_queries": 50,
    "valid_queries": 48
  }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `node` | string | 语法规则名 |
| `sqlcom_values` | []int | 观察到的所有 SQLCOM 整数值（去重，升序） |
| `sqlcom_names` | []string | 对应的 `SQLCOM_*` 枚举名称 |
| `dominant_sqlcom` | int | 出现频率最高的 SQLCOM 值；-1 表示未映射 |
| `dominant_sqlcom_name` | string | `dominant_sqlcom` 对应的枚举名称 |
| `stats.sqlcom_freq` | map | SQLCOM 值 → 出现次数 |
| `stats.status_freq` | map | 执行状态 → 出现次数 |
| `stats.total_queries` | int | 该节点生成的总查询数 |
| `stats.valid_queries` | int | 执行状态为 `normal` 的查询数 |

执行状态（`status`）的含义：

| 状态 | 含义 |
|------|------|
| `normal` | 执行成功，已捕获 SQLCOM |
| `syntax_error` | MySQL 报 `ER_PARSE_ERROR`（SQL 语法错误） |
| `semantic_error` | 执行失败，但非语法错误（如表不存在） |
| `crash` | MySQL 服务器崩溃（连接丢失） |
| `no_sqlcom` | 执行成功/失败但超时未在日志中捕获到 SQLCOM |

#### `sqlcom_to_nodes` 反向索引

以 SQLCOM 名称为键，列出所有 `dominant_sqlcom` 为该值的节点列表：

```json
{
  "SQLCOM_SELECT": ["select_stmt"],
  "SQLCOM_INSERT": ["insert_stmt"],
  ...
}
```

---

## 当前映射结果（N=50 运行）

本次运行共探测 **135 个** `simple_statement` 直接子规则，成功映射 **122 个**。

### 典型映射示例

| 语法节点 | 映射的 SQLCOM | 说明 |
|----------|---------------|------|
| `select_stmt` | `SQLCOM_SELECT` (0) | |
| `insert_stmt` | `SQLCOM_INSERT` (5) | |
| `delete_stmt` | `SQLCOM_DELETE` (7) | |
| `create_table_stmt` | `SQLCOM_CREATE_TABLE` (1) | |
| `drop_table_stmt` | `SQLCOM_DROP_TABLE` (9) | |
| `alter_table_stmt` | `SQLCOM_ALTER_TABLE` (3) | |

### 有趣的映射（与直觉不符）

| 语法节点 | 实际映射 | 原因 |
|----------|----------|------|
| `update_stmt` | `SQLCOM_UPDATE_MULTI` (75) | RSG 倾向于生成多表 `UPDATE ... JOIN` 形式 |
| `explain_stmt` | `SQLCOM_UPDATE` (4) | RSG 在 `EXPLAIN` 内部生成了 `UPDATE` 语句 |
| `rollback` | `SQLCOM_ROLLBACK_TO_SAVEPOINT` (55) | RSG 倾向于生成带 `SAVEPOINT` 的完整形式 |

### 未能映射的节点（13 个）

| 节点 | 原因 |
|------|------|
| `alter_database_stmt`, `alter_user_stmt`, `drop_database_stmt`, `drop_user_stmt`, `grant`, `revoke`, `set`, `set_role_stmt`, `shutdown_stmt` | `GenerateMySQL` 内部黑名单，生成 0 条 SQL |
| `get_diagnostics`, `show_count_errors_stmt`, `show_count_warnings_stmt`, `signal_stmt` | 生成了 SQL，但 MySQL 在 `sql_parse.cc:5327` 之前就返回，无法捕获 SQLCOM |

---

## 已知问题

### mysqld 崩溃恢复

在 N=50 的运行中共观察到 **14 次 mysqld 崩溃**。执行器实现了自动重连机制：

- 检测到崩溃（`CR_SERVER_LOST` / `CR_SERVER_GONE_ERROR`）后，
  等待最多 30 秒（30 次 × 1 秒）尝试重连
- 若重连成功，继续执行后续查询

已确认的崩溃触发 SQL：

```sql
-- 稳定复现崩溃（MySQL 8.0.33）
CREATE ROLE 'abc';
```

调用栈：`mysql_create_user` → `Sql_cmd_create_role::execute`

> 该 crash 是 MySQL 8.0.33 在特定补丁配置下的真实 bug，与 node_mapper 无关。

### `COMMIT AND CHAIN NO RELEASE`

执行此语句会导致客户端连接断开（错误码 `CR_SERVER_LOST=2013`），
但 mysqld 本身不会崩溃。执行器的重连逻辑会正确处理此情况。

---

## 性能参考

| 参数 | N=50 实测 |
|------|-----------|
| 总查询数 | ~6,300 |
| 运行时长 | ~2.5 分钟 |
| 崩溃次数 | 14 次（每次约耗时 30 秒重连） |
| 映射成功率 | 122/135（90%） |

> 提高 `-poll-timeout-ms` 可减少 `no_sqlcom` 漏判，但会增加总运行时间。
> 默认值 100ms 在本机实测中是较好的平衡点。

---

## 源码说明

### `simple_rsg.go` — 轻量级 SQL 生成器

`SimpleRSG` 是专为 node_mapper 设计的精简 RSG 实现，不包含 MAB（多臂老虎机）
和 coverage feedback 机制。主要特点：

- **边分类（edge classification）**：将每条产生式规则分为 `terminal`（终结符）、
  `normal`（普通非终结符）、`complex`（含复杂子表达式）三类
- **深度限制**：当递归深度耗尽时，优先选择终结符规则，防止无限递归
- **FuzzingMode=3**：激活 `GenerateMySQL` 的深度截断分支

### `main.go` — 编排主程序

主程序流程：
1. 解析 `.y` 语法文件，枚举 `simple_statement` 的直接子规则
2. 调用 `SimpleRSG.GenerateSQL` 为每个规则生成 N 条 SQL
3. 将所有查询写入 `queries.tsv`
4. 启动 C++ executor 子进程，传入参数，读取结果
5. 汇总统计并写出 `node_sqlcom_mapping.json`

### `executor/main.cpp` — C++ MySQL 执行器

执行器的核心设计要点：

1. **SQLCOM 捕获策略**：在执行 SQL **前**记录日志文件大小，执行后轮询新增内容
   直到找到 `SQLCOM_EXEC: N` 行或超时

2. **崩溃检测修复**：在调用 `cleanUpConnection`（内部调用 `mysql_next_result`）
   **之前**立即保存 `mysql_errno`。原因：`mysql_next_result` 返回 -1 时会
   将 errno 重置为 0，导致崩溃检测失效

3. **自动重连**：检测到崩溃后，以 1 秒间隔重试最多 30 次，等待 mysqld 重启
