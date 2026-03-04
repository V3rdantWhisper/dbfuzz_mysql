# node_mapper — MySQL 语法节点到 SQL 命令映射发现工具

## 概述

`node_mapper` 是一个自动化工具，用于发现 MySQL 语法文件（`.y`）中的语法规则（grammar
rule）与 MySQL 内部枚举类型 `enum_sql_command` 之间的对应关系。

工具解决两个层次的问题：

1. **顶层映射**：`simple_statement` 的直接子规则（如 `insert_stmt`、`create` 等）
   生成的 SQL 对应哪个 `SQLCOM_*` 枚举值？

2. **差分映射**：对于**所有语法规则**（不仅是顶层规则），当某条规则
   *被命中*（生成的 SQL 包含其特征关键词）时，与*未命中*时相比，
   `SQLCOM_*` 分布有何差异？——即该规则对 SQL 命令的贡献是什么？

这一映射关系可用于：

- 引导模糊测试器（fuzzer）定向生成特定 SQL 命令类型
- 分析语法规则的覆盖情况与命令贡献
- 辅助 MySQL 源码审计与行为分析

---

## 工作原理

### 阶段一：顶层节点探测

```
语法文件 (mysql_sql.y)
        │
        ▼
  [1] 解析语法，枚举 simple_statement 的所有直接子规则（135 个）
      + 始终包含 begin_stmt（位于 simple_statement_or_begin，共 136 个根节点）
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
  [4] Go 端汇总统计，写入 JSON 的 mappings 数组
```

### 阶段二：差分节点分析（`-diff-test`）

对 **所有** 语法规则（不仅是 `simple_statement` 的直接子规则），通过
"包含该节点" vs "不包含该节点" 的对比来推断其命令贡献：

```
对每条非顶层语法规则 X：
        │
        ├─ [A] 用 RSG 生成最多 20 条碎片 SQL（X 作为生成根）
        │      → 提取出现最一致的 SQL 关键词作为"判别词"（Discriminator）
        │
        ├─ [B] 在语法图中沿父节点方向 BFS
        │      → 找到最近的 simple_statement 子规则祖先 P
        │
        ├─ [C] 以 P 为生成根，生成两批各 diff-n 条 SQL：
        │        WITH    标签：SQL 中包含判别词   → "X:WITH"
        │        WITHOUT 标签：SQL 中不含判别词   → "X:WITHOUT"
        │
        └─ [D] 执行后对比两组 SQLCOM 分布
               only_in_with    = "WITH" 独有的命令 → X 贡献的命令
               only_in_without = "WITHOUT" 独有的命令 → X 排除的命令
```

差分结果写入 JSON 的 `diff_mappings` 数组（需启用 `-diff-test`）。

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
├── main.go                   # Go 主程序：语法解析、SQL 生成、差分分析、JSON 输出
├── simple_rsg.go             # SimpleRSG：轻量级随机 SQL 生成器（无 MAB/反馈）
├── Makefile                  # 构建与运行入口
├── setup_mysql.sh            # mysqld 启动/停止/重启辅助脚本
├── executor/
│   └── main.cpp              # C++ 执行器：连接 MySQL、捕获 SQLCOM_EXEC
├── node_sqlcom_mapping.json  # 最新一次运行的输出
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

patched mysqld 位置：

```
mysql-server-mysql-8.0.33/bld/runtime_output_directory/mysqld
```

---

## 环境准备：启动 MySQL

工具需要一个**已运行的 patched MySQL 实例**，且其 stderr 被重定向到日志文件。
可使用项目提供的 `setup_mysql.sh` 辅助脚本（推荐）：

```bash
./setup_mysql.sh start    # 启动
./setup_mysql.sh stop     # 停止
./setup_mysql.sh restart  # 重启
./setup_mysql.sh status   # 查看状态
```

或手动启动：

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
  2>>/tmp/mysql_stderr.log &
```

> **关键**：`2>>/tmp/mysql_stderr.log` 必须存在，工具通过轮询该文件读取
> `SQLCOM_EXEC` 输出。若文件路径不同，通过 `-log-file` 参数指定。

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

**标准运行**（仅顶层节点探测）：

```bash
make run
```

**启用差分分析**：

```bash
make run DIFF_TEST=true DIFF_N=50
```

可覆盖的所有 Makefile 变量：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `N` | `200` | 每个顶层节点生成的 SQL 数量 |
| `DEPTH` | `2` | RSG 递归生成深度 |
| `LOG_FILE` | `/tmp/mysql_stderr.log` | mysqld stderr 日志路径 |
| `GRAMMAR` | `../parser_def_files/mysql_sql.y` | MySQL 语法文件路径 |
| `OUTPUT` | `./node_sqlcom_mapping.json` | 输出 JSON 路径 |
| `MYSQL_HOST` | `127.0.0.1` | MySQL 主机 |
| `MYSQL_PORT` | `3306` | MySQL 端口 |
| `MYSQL_USER` | `root` | MySQL 用户名 |
| `MYSQL_PASS` | _(空)_ | MySQL 密码 |
| `MYSQL_DB` | `node_mapper_db` | 工作数据库名（运行期间自动创建/删除） |
| `SETUP_SCRIPT` | `./setup_mysql.sh` | mysqld 崩溃后的重启脚本路径 |
| `MAX_QUICK_RETRIES` | `3` | 触发 mysqld 重启前的快速重连次数 |
| `DIFF_TEST` | `false` | 设为 `true` 开启差分节点分析 |
| `DIFF_N` | `20` | 差分分析每个方向（WITH/WITHOUT）的查询数 |

### 方式二：直接调用 Go 二进制

完整参数列表：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-grammar` | `../parser_def_files/mysql_sql.y` | MySQL `.y` 语法文件路径 |
| `-executor` | `./executor/executor` | C++ 执行器二进制路径 |
| `-log-file` | `/tmp/mysql_stderr.log` | mysqld stderr 日志文件路径 |
| `-output` | `./node_sqlcom_mapping.json` | 输出 JSON 文件路径 |
| `-queries-file` | `./queries.tsv` | 中间 TSV 查询文件路径 |
| `-n` | `200` | 每个顶层节点生成的 SQL 查询数 |
| `-depth` | `2` | RSG 生成深度 |
| `-seed` | `0`（时间戳） | 随机种子（0 表示用系统时间） |
| `-extra-roots` | _(空)_ | 额外探测的根节点，逗号分隔 |
| `-host` | `127.0.0.1` | MySQL 主机 |
| `-port` | `3306` | MySQL 端口 |
| `-user` | `root` | MySQL 用户名 |
| `-password` | _(空)_ | MySQL 密码 |
| `-database` | `node_mapper_db` | 工作数据库名 |
| `-poll-timeout-ms` | `100` | 等待 `SQLCOM_EXEC` 输出的最大毫秒数 |
| `-setup-script` | `./setup_mysql.sh` | mysqld 崩溃后用于重启的脚本路径 |
| `-max-quick-retries` | `3` | 触发 mysqld 重启前的快速重连尝试次数 |
| `-diff-test` | `false` | 开启差分节点分析 |
| `-diff-n` | `20` | 差分分析每个方向的查询数 |

示例：

```bash
# 以固定种子运行，每节点 100 条 SQL
./node_mapper -n 100 -seed 42 -output ./mapping_n100.json

# 启用差分分析，每方向 50 条
./node_mapper -n 200 -diff-test -diff-n 50

# 探测额外节点
./node_mapper -extra-roots "xa,create"
```

---

## 输出文件说明

### `queries.tsv`

每行一条查询，格式为 `节点名<TAB>SQL语句`：

```
insert_stmt	INSERT INTO t ( a ) VALUES ( 1 )
create	CREATE DATABASE v0
begin_stmt	BEGIN WORK
view_tail:WITH	VIEW v0 AS SELECT TRUE
view_tail:WITHOUT	CREATE TABLE v0 ( a INT )
```

差分查询的节点名格式为 `规则名:WITH` 或 `规则名:WITHOUT`，以 `:` 与普通节点区分。

### `node_sqlcom_mapping.json`

顶层结构：

```json
{
  "generated_at": "2026-03-04T...",
  "grammar_file": "../parser_def_files/mysql_sql.y",
  "queries_per_node": 200,
  "mappings": [ ... ],
  "diff_mappings": [ ... ],
  "sqlcom_to_nodes": { ... }
}
```

`diff_mappings` 仅在使用 `-diff-test` 时出现，否则该字段被省略。

#### `mappings` 数组（顶层探测结果）

每个元素对应一个语法节点：

```json
{
  "node": "insert_stmt",
  "sqlcom_values": [5],
  "sqlcom_names":  ["SQLCOM_INSERT"],
  "dominant_sqlcom": 5,
  "dominant_sqlcom_name": "SQLCOM_INSERT",
  "stats": {
    "sqlcom_freq": { "5": 197 },
    "status_freq": { "normal": 197, "semantic_error": 3 },
    "total_queries": 200,
    "valid_queries": 197
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

#### `diff_mappings` 数组（差分分析结果）

每个元素对应一条非顶层语法规则：

```json
{
  "node": "view_tail",
  "parent_root": "create",
  "discriminator": "VIEW",
  "with_sqlcoms":    ["SQLCOM_CREATE_VIEW"],
  "without_sqlcoms": ["SQLCOM_ALTER_TABLESPACE", "SQLCOM_CREATE_DB",
                      "SQLCOM_CREATE_FUNCTION", "SQLCOM_CREATE_SERVER", ...],
  "only_in_with":    ["SQLCOM_CREATE_VIEW"],
  "only_in_without": ["SQLCOM_ALTER_TABLESPACE", "SQLCOM_CREATE_DB", ...],
  "with_total": 20, "with_valid": 18,
  "without_total": 20, "without_valid": 19
}
```

| 字段 | 说明 |
|------|------|
| `node` | 被分析的语法规则名 |
| `parent_root` | 用于生成完整 SQL 的顶层祖先节点 |
| `discriminator` | 作为判别词的 SQL 关键词 |
| `with_sqlcoms` | 包含判别词时观察到的全部 SQLCOM 名称 |
| `without_sqlcoms` | 不含判别词时观察到的全部 SQLCOM 名称 |
| `only_in_with` | **仅**在含判别词时出现的 SQLCOM（即该节点贡献的命令） |
| `only_in_without` | 仅在不含判别词时出现的 SQLCOM（即该节点排除的命令） |
| `with_total` / `with_valid` | WITH 方向的总查询数 / 正常执行数 |
| `without_total` / `without_valid` | WITHOUT 方向的总查询数 / 正常执行数 |

#### `sqlcom_to_nodes` 反向索引

以 SQLCOM 名称为键，列出所有 `dominant_sqlcom` 为该值的节点：

```json
{
  "SQLCOM_SELECT":       ["select_stmt"],
  "SQLCOM_INSERT":       ["insert_stmt"],
  "SQLCOM_CREATE_TABLE": ["create_table_stmt"],
  ...
}
```

---

## 当前映射结果（N=200 运行）

本次运行共探测 **136 个**根节点（135 个 `simple_statement` 直接子规则 +
`begin_stmt`），成功映射 **126 个**（93%）。
差分分析从 961 条语法规则中自动构建了 **482 个** DiffSpec。

### 有趣的映射（与直觉不符）

| 语法节点 | 实际映射 | 原因 |
|----------|----------|------|
| `update_stmt` | `SQLCOM_UPDATE_MULTI` | RSG 倾向于生成多表 `UPDATE ... JOIN` 形式 |
| `explain_stmt` | `SQLCOM_UPDATE` | RSG 在 `EXPLAIN` 内部生成了 `UPDATE` 语句 |
| `rollback` | `SQLCOM_ROLLBACK_TO_SAVEPOINT` | RSG 倾向于生成带 `TO SAVEPOINT` 的完整形式 |

### 未能映射的顶层节点

| 节点 | 原因 |
|------|------|
| `alter_database_stmt`、`alter_user_stmt`、`drop_database_stmt`、`drop_user_stmt`、`grant`、`revoke`、`set`、`set_role_stmt`、`shutdown_stmt` | `GenerateMySQL` 内部黑名单，生成 0 条 SQL |
| `get_diagnostics`、`show_count_errors_stmt`、`show_count_warnings_stmt`、`signal_stmt` | 生成了 SQL，但 MySQL 在 `sql_parse.cc:5327` 之前就已返回，无法捕获 SQLCOM |

### 差分分析典型结论

启用 `-diff-test` 后，`only_in_with` 非空的节点即为真正意义上能 **唯一决定** SQL
命令的中间规则。预期有价值的节点示例：

| 节点 | 判别词 | 预期 only_in_with |
|------|--------|-------------------|
| `view_tail` | `VIEW` | `SQLCOM_CREATE_VIEW` |
| `xa` 的子规则 | `BEGIN`/`END`/`PREPARE`... | 各 XA 子命令 |
| `sp_tail` | `PROCEDURE` | `SQLCOM_CREATE_PROCEDURE` |
| `sf_tail` | `FUNCTION` | `SQLCOM_CREATE_SPFUNCTION` |

---

## 已知问题

### mysqld 崩溃恢复

执行器实现了自动重连与 mysqld 重启机制：

- 检测到 `CR_SERVER_LOST` / `CR_SERVER_GONE_ERROR` 后，以 1 秒间隔快速重连
  最多 `max-quick-retries`（默认 3）次
- 快速重连失败后，调用 `setup-script restart` 重启 mysqld，再等待最多 30 秒

已确认的崩溃触发 SQL（MySQL 8.0.33）：

```sql
CREATE ROLE 'abc';   -- 调用栈：mysql_create_user → Sql_cmd_create_role::execute
```

> 该 crash 是 MySQL 8.0.33 在特定补丁配置下的真实 bug，与 node_mapper 无关。

### `COMMIT AND CHAIN NO RELEASE`

执行此语句会导致客户端连接断开（`CR_SERVER_LOST=2013`），但 mysqld 本身不会崩溃。
执行器的快速重连逻辑会正确处理此情况，无需重启 mysqld。

---

## 性能参考

| 参数 | N=200（顶层） | N=200 + diff-n=20（差分） |
|------|--------------|--------------------------|
| 总查询数 | ~25,200 | ~50,000+ |
| 预计运行时长 | ~10 分钟 | ~25 分钟 |
| 崩溃次数（典型） | ~54 次（全部快速恢复） | 相近 |
| 顶层映射成功率 | 126/136（93%） | 相同 |
| 差分 spec 数量 | — | 482 |

> 提高 `-poll-timeout-ms` 可减少 `no_sqlcom` 漏判，但会增加总运行时间。
> 默认值 100ms 在本机实测中是较好的平衡点。

---

## 源码说明

### `simple_rsg.go` — 轻量级 SQL 生成器

`SimpleRSG` 是专为 node_mapper 设计的精简 RSG 实现，不包含 MAB 和 coverage
feedback 机制。主要特点：

- **边分类**：将每条产生式分为 `terminal`（终结符）、`normal`（普通非终结符）、
  `complex`（含复杂子表达式）三类
- **深度限制**：递归深度耗尽时优先选择终结符规则，防止无限递归
- **FuzzingMode=3**：激活 `GenerateMySQL` 的深度截断分支

### `main.go` — 编排主程序

核心函数：

| 函数 | 说明 |
|------|------|
| `directChildren` | 枚举 `simple_statement` 的单 token 产生式子规则 |
| `buildChildToParents` | 构建语法图的反向依赖映射（子规则 → 父规则列表） |
| `findTopLevelAncestor` | 在反向图上 BFS，找到最近的顶层祖先节点 |
| `isKeywordLike` | 判断一个词是否为全大写 ASCII 字母（SQL 关键词特征） |
| `extractDiscriminator` | 生成碎片样本，提取出现最一致的 SQL 关键词作为判别词 |
| `buildDiffSpecs` | 对所有非顶层规则批量构建 `DiffSpec`（调用以上两函数） |
| `generateQueries` | 顶层节点 SQL 批量生成 |
| `generateDiffQueries` | 按 DiffSpec 生成 WITH/WITHOUT 两组查询 |
| `runExecutor` | 启动 C++ 执行器子进程，读取结果 |
| `buildMapping` | 汇总顶层节点执行结果，构建 `MappingEntry` 列表 |
| `buildDiffMapping` | 汇总差分结果，计算 `OnlyInWith`/`OnlyInWithout` |

主程序流程：

```
解析语法 → 构建 RSG
        ↓
枚举 136 个根节点 + (可选) 构建 482 个差分 Spec
        ↓
生成 SQL（顶层 + 差分 WITH/WITHOUT）→ 写入 queries.tsv
        ↓
C++ executor 执行全部查询 → 收集 (node, sqlcom, status)
        ↓
按节点名是否含 ":" 分流：正常结果 / 差分结果
        ↓
buildMapping(正常) + buildDiffMapping(差分) → JSON
```

### `executor/main.cpp` — C++ MySQL 执行器

核心设计：

1. **SQLCOM 捕获**：执行 SQL **前**记录日志文件大小，执行后轮询新增内容，
   直到找到 `SQLCOM_EXEC: N` 行或超时（`-poll-timeout-ms`）

2. **崩溃检测修复**：在调用 `cleanUpConnection`（内部调用 `mysql_next_result`）
   **之前**立即保存 `mysql_errno`，因为 `mysql_next_result` 返回 -1 时会
   将 errno 重置为 0

3. **两级恢复**：
   - 快速重连（`--max-quick-retries` 次，间隔 1 秒）
   - 硬重启（调用 `setup-script restart`，等待最多 30 秒）
