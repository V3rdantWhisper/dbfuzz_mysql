# node_sqlcom_mapping.json 分析报告

**运行参数**：`N=200, depth=2, diff-test=true, diff-n=20`  
**生成时间**：2026-03-04  
**数据文件**：`node_sqlcom_mapping.json`（11,692行）

---

## 一、概览数据

| 指标 | 数值 |
|---|---|
| 探测的 grammar 节点总数 | 136（135 direct children + `begin_stmt`） |
| 有 dominant SQLCOM 的节点 | 123 |
| 无有效 SQL（空状态）的节点 | 9（RSG blacklisted） |
| 返回 no_sqlcom 的节点 | 4 |
| diff_mappings 条目总数 | 450 |
| diff_mappings 中有 `only_in_with` 的条目 | 51 |
| `enum_sql_command` 总枚举值（去掉 `SQLCOM_END`） | 159 |
| 常规映射中见到的 SQLCOM | 139 |
| 合并 diff 后见到的 SQLCOM | 141 |
| 任何途径都未见到的 SQLCOM | **18** |

---

## 二、对照 mysql.y 的理论分析：判定结果是否符合预期

### 2.1 单一映射节点（预期正确，实测吻合）

绝大多数 `simple_statement` 直接子节点的映射结果与语法文件完全一致：

| 节点 | 实测 dominant | 理论依据（mysql.y 行） |
|---|---|---|
| `commit` | `SQLCOM_COMMIT` | L15053 |
| `rollback` | `SQLCOM_ROLLBACK` | L15065 |
| `begin_stmt` | `SQLCOM_BEGIN` | L7123 |
| `start` | `SQLCOM_BEGIN` | `start` 生成 `START TRANSACTION`，MySQL 内部等价于 BEGIN |
| `create_table_stmt` | `SQLCOM_CREATE_TABLE` | C++ parser（非 .y action） |
| `alter_table_stmt` | `SQLCOM_ALTER_TABLE` | C++ parser |
| `select_stmt` | `SQLCOM_SELECT` | C++ parser |
| `insert_stmt` | `SQLCOM_INSERT` | C++ parser |
| `purge` | `SQLCOM_PURGE` | L12111（差别见 §2.3） |
| `xa` | `SQLCOM_XA_RECOVER`（dominant） | 六种 XA_* 均匀分布，dominant 仅因随机多出一票 |

### 2.2 多值映射节点（一个节点→多个 SQLCOM，符合预期）

这些节点是语法上的"分发节点"，在不同展开路径下触发不同命令：

| 节点 | 实测所有 SQLCOM | 解释 |
|---|---|---|
| `xa` | XA_START/END/PREPARE/COMMIT/ROLLBACK/RECOVER（6种，~均匀） | 一条规则包含全部 XA 子句 |
| `group_replication` | START_GROUP_REPLICATION / STOP_GROUP_REPLICATION | 两个子句各约 50% |
| `delete_stmt` | DELETE / DELETE_MULTI | 单表/多表 DELETE 共用同一语法节点 |
| `update_stmt` | UPDATE / UPDATE_MULTI | 单表/多表 UPDATE 共用同一语法节点 |
| `lock` | LOCK_TABLES / LOCK_INSTANCE | 两种 LOCK 语法同属 lock 节点 |
| `unlock` | UNLOCK_TABLES / UNLOCK_INSTANCE | 同上 |
| `install_stmt` | INSTALL_PLUGIN / INSTALL_COMPONENT | 两种安装语法 |
| `uninstall` | UNINSTALL_PLUGIN / UNINSTALL_COMPONENT | 两种卸载语法 |
| `rename` | RENAME_TABLE / RENAME_USER | `RENAME TABLE` 和 `RENAME USER` 共用 rename 节点 |
| `create` | CREATE_DB / CREATE_FUNCTION / CREATE_USER / CREATE_VIEW / CREATE_TRIGGER / ALTER_TABLESPACE / CREATE_SERVER（7种） | `create` 是所有 CREATE 语句的公共前缀节点 |

### 2.3 需要特别说明的映射

#### `alter_view_stmt` → `SQLCOM_CREATE_VIEW`

**看起来错误，实际正确。**  
MySQL 8.0 没有独立的 `SQLCOM_ALTER_VIEW`。`ALTER VIEW` 在语法和执行层面被实现为 `CREATE OR REPLACE VIEW`，grammar 文件 L15257 确认 `view_tail` 的 action 直接写入 `SQLCOM_CREATE_VIEW`。

#### `explain_stmt` → `SQLCOM_UPDATE`（dominant，占 25%）

**这是 dominant 误判，不影响覆盖正确性。**  
`explain_stmt` 本身产生 `SQLCOM_EXPLAIN_OTHER`（当解释非活跃查询时），但 RSG 以 depth=2 生成时多数展开为被解释的 DML 查询，导致实际执行的是被解释的语句而非 EXPLAIN 本身。76 条有效结果中分布如下：

```
UPDATE        25.0%  (19)  <- RSG 随机主导，产生 false dominant
SELECT        17.1%  (13)
EXPLAIN_OTHER 14.5%  (11)  <- 才是 explain_stmt 本身的真实映射
INSERT        14.5%  (11)
...
```

**建议**：`explain_stmt` 的 dominant 应手动修正为 `SQLCOM_EXPLAIN_OTHER`，或将 N 提升到 1000+ 以稳定结果。

#### `create` → `SQLCOM_ALTER_TABLESPACE`（dominant，69/130）

**RSG 概率分布的产物，非 bug。**  
`create` 节点展开路径众多，RSG 在 depth=2 下恰好最常生成 tablespace 相关语法（CREATE LOGFILE GROUP / CREATE TABLESPACE），导致 `ALTER_TABLESPACE` 频次最高。这是 RSG 生成分布不均的正常现象，不影响覆盖的正确性——7 种 SQLCOM 均被观察到。

#### `start` → `SQLCOM_BEGIN`

**正确**，不是 BEGIN 的 dominant 映射之争。`start` 规则只生成 `START TRANSACTION`，MySQL 内部 `START TRANSACTION` 等价于 `BEGIN`，两者共用 `SQLCOM_BEGIN`（L7123）。

### 2.4 显著正确的 diff_mappings 验证样例

| diff 节点 | discriminator | only_in_with | 理论验证 |
|---|---|---|---|
| `purge_option` | `BEFORE` | `SQLCOM_PURGE_BEFORE` | L12111：`PURGE ... BEFORE` → PURGE_BEFORE |
| `sp_tail` | `PROCEDURE` | `SQLCOM_CREATE_PROCEDURE` | L15720 |
| `sf_tail` | `FUNCTION` | `SQLCOM_CREATE_FUNCTION` | L15622（实为 sp function） |
| `trigger_tail` | `TRIGGER` | `SQLCOM_CREATE_TRIGGER` | L15429 |
| `group_replication_start` | `START` | `SQLCOM_START_GROUP_REPLICATION` | 正确区分 START/STOP |
| `rename_list` | `TO` | RENAME_TABLE + RENAME_USER | L7475/L7481 均覆盖 |
| `create_user` | `IDENTIFIED` | `SQLCOM_CREATE_USER` | L14259 |

---

## 三、我们的映射结果还有哪些遗漏的 SQLCOM？

### 3.1 整体覆盖情况

```
enum_sql_command 总量（去 SQLCOM_END）:  159
常规映射见到:                            139 (87.4%)
加上 diff_mappings 后:                   141 (88.7%)
仍未见到:                                 18 (11.3%)
```

### 3.2 18 个仍未见到的 SQLCOM 分类

#### A 类：RSG Blacklisted（9个）—— 对应节点 `GenerateMySQL` 返回空

| 未见 SQLCOM | 对应节点 | 原因 |
|---|---|---|
| `SQLCOM_ALTER_DB` | `alter_database_stmt` | RSG 黑名单，生成 0 条 SQL |
| `SQLCOM_ALTER_USER` | `alter_user_stmt` | RSG 黑名单 |
| `SQLCOM_DROP_DB` | `drop_database_stmt` | RSG 黑名单 |
| `SQLCOM_DROP_USER` | `drop_user_stmt` | RSG 黑名单 |
| `SQLCOM_GRANT` | `grant` | RSG 黑名单 |
| `SQLCOM_REVOKE` | `revoke` | RSG 黑名单 |
| `SQLCOM_REVOKE_ALL` | `revoke` | RSG 黑名单（与 REVOKE 共节点） |
| `SQLCOM_SET_ROLE` | `set_role_stmt` | RSG 黑名单 |
| `SQLCOM_SHUTDOWN` | `shutdown_stmt` | RSG 黑名单 |

**根本原因**：`simple_rsg.go / mysqlGenerator.go` 中 blacklist 包含了权限和破坏性语句，是有意设计，防止测试实例被破坏。

#### B 类：Patch 不触发（2个）—— SQL 执行路径在 instrumentation 点之前返回

| 未见 SQLCOM | 对应节点 | 现象 | 原因 |
|---|---|---|---|
| `SQLCOM_GET_DIAGNOSTICS` | `get_diagnostics` | 200 条全是 `no_sqlcom` | `GET DIAGNOSTICS` 在条件处理器中执行，`sql_parse.cc:5327` 不被触发 |
| `SQLCOM_SIGNAL` | `signal_stmt` | 200 条全是 `no_sqlcom` | `SIGNAL` 为存储过程上下文语句，同上 |

#### C 类：CREATE_SPFUNCTION 缺口（1个）

| 未见 SQLCOM | 分析 |
|---|---|
| `SQLCOM_CREATE_SPFUNCTION` | `create` 节点确实可生成 `CREATE FUNCTION ... SONAME '...'`（UDF，外部函数），对应 `udf_tail`，但 diff_mappings 中 `udf_tail` 的 `only_in_with` 仍返回 `SQLCOM_CREATE_FUNCTION`（native function）而非 `SPFUNCTION`。原因：RSG depth=2 对 `udf_tail` 展开无法生成有效的 SONAME 路径（需要合法动态库名），MySQL 解析器可能直接降级到 `SQLCOM_CREATE_FUNCTION`。 |

#### D 类：纯 C++ 赋值（mysql.y 中从未出现 `sql_command=`）（5个）

| 未见 SQLCOM | 赋值位置 | 说明 |
|---|---|---|
| `SQLCOM_ALTER_USER_DEFAULT_ROLE` | `sql/sql_parse.cc` 或 `sql/parse_tree_nodes.cc` | `ALTER USER ... DEFAULT ROLE` 由 PT 节点在 C++ 层设置 |
| `SQLCOM_GRANT_ROLE` | C++ parser | `GRANT role TO user` 语法，C++ 层识别 |
| `SQLCOM_REVOKE_ROLE` | C++ parser | `REVOKE role FROM user` 语法 |
| `SQLCOM_SET_OPTION` | C++ parser | `SET var=val` 的通用 SET handler |
| `SQLCOM_SET_PASSWORD` | 仅出现在 .y 的 guard 条件中（L2741-2744），**从未被赋值** | `SET PASSWORD` / `ALTER USER ... IDENTIFIED BY` 通过 `alter_user_stmt` 处理，但 alter_user_stmt 被 RSG 黑名单拦截 |

#### E 类：EMPTY_QUERY（1个）

`SQLCOM_EMPTY_QUERY` 仅由空输入触发（`thd->lex->sql_command = SQLCOM_EMPTY_QUERY` 在 L58，仅当解析器接到空语句时执行）。RSG 无法生成空 SQL，也不属于任何 grammar 节点，理论上不可通过现有框架捕获。

---

## 四、mysql.y 中不含赋值的 SQLCOM —— 我们能否获取它们？

### 4.1 背景

通过统计 mysql.y 中所有 `lex->sql_command= SQLCOM_xxx` 赋值语句，得到在 grammar 文件中**直接赋值**的 SQLCOM 共 **76 个**。其余 **83 个**是在 C++ 代码（`sql_parse.cc`、`parse_tree_nodes.cc`、`parse_tree_statements.cc` 等）中赋值的——mysql.y 只做语法解析树构建，最终命令由 `itemize()/prepare()` 阶段的 PT 节点决定。

### 4.2 我们的实际覆盖

尽管这 83 个 SQLCOM 在 .y 文件中没有 `sql_command=` 赋值，我们**已经成功捕获其中 78 个**（94%），原因是：mysqld 的 stderr patch 是在 `mysql_execute_command()` 入口处打印，只要语句最终进入执行流，无论由哪一层设置 `sql_command`，都能被捕获。

**未捕获的 5 个**（均为 C++ 赋值且无 RSG 路径）：

| SQLCOM | mysql.y 中的状态 | 为什么没捕获 |
|---|---|---|
| `SQLCOM_ALTER_USER_DEFAULT_ROLE` | 完全不出现 | `ALTER USER ... DEFAULT ROLE` 需要 `alter_user_stmt`，但该节点被 RSG 黑名单拦截 |
| `SQLCOM_GRANT_ROLE` | 完全不出现 | `GRANT role TO user` 需要 `grant` 节点，RSG 黑名单 |
| `SQLCOM_REVOKE_ROLE` | 完全不出现 | `REVOKE role FROM user` 需要 `revoke` 节点，RSG 黑名单 |
| `SQLCOM_SET_OPTION` | 仅作 guard 条件 | `set` 节点被 RSG 黑名单拦截 |
| `SQLCOM_SET_PASSWORD` | 仅作 guard 条件 | 同上，依赖 `alter_user_stmt` 或 `set` |

### 4.3 能否获取这些未见 SQLCOM？

| SQLCOM | 能否获取 | 方案 |
|---|---|---|
| `SQLCOM_ALTER_USER_DEFAULT_ROLE` | **能** | 手工注入 SQL：`ALTER USER 'root'@'localhost' DEFAULT ROLE NONE`，直接写入 queries.tsv 或添加硬编码 seed |
| `SQLCOM_GRANT_ROLE` | **能** | 手工注入：`GRANT r TO u`（需先创建 role/user）；或解除 `grant` 节点的 blacklist |
| `SQLCOM_REVOKE_ROLE` | **能** | 同上，`REVOKE r FROM u` |
| `SQLCOM_SET_OPTION` | **能** | `SET @a=1` 或 `SET SESSION sql_mode='STRICT'`；解除 `set` 节点的 blacklist |
| `SQLCOM_SET_PASSWORD` | **能** | `ALTER USER 'root'@'localhost' IDENTIFIED BY 'x'`；需要解除 `alter_user_stmt` blacklist 或手工注入 |
| `SQLCOM_SHUTDOWN` | **能（有代价）** | 手工注入 `SHUTDOWN`，但会终止 mysqld，需要在独立测试中处理 |
| `SQLCOM_GRANT` | **能** | 解除 `grant` blacklist，但可能修改权限导致后续测试失败 |
| `SQLCOM_REVOKE`/`REVOKE_ALL` | **能** | 同上 |
| `SQLCOM_ALTER_DB` | **能** | `ALTER DATABASE db CHARACTER SET utf8mb4` |
| `SQLCOM_DROP_DB` | **能（有代价）** | `DROP DATABASE x`，需要 setup 环节先建库 |
| `SQLCOM_ALTER_USER` | **能** | `ALTER USER 'root'@'localhost' ACCOUNT UNLOCK` |
| `SQLCOM_DROP_USER` | **能（有代价）** | 需要先创建测试用户 |
| `SQLCOM_SET_ROLE` | **能** | `SET ROLE NONE` / `SET DEFAULT ROLE NONE TO user` |
| `SQLCOM_GET_DIAGNOSTICS` | **不能**（patch 问题） | 需要修改 mysqld patch 的插桩位置 |
| `SQLCOM_SIGNAL` | **不能**（patch 问题） | 同上，仅在 SP 上下文内触发 |
| `SQLCOM_CREATE_SPFUNCTION` | **能** | `CREATE FUNCTION fname RETURNS INT SONAME 'libfoo.so'` 手工注入 |
| `SQLCOM_EMPTY_QUERY` | **不能** | 架构限制：RSG 无法生成空 SQL |

---

## 五、总结

### 覆盖结论

| 类别 | 数量 | 状态 |
|---|---|---|
| 完全正确映射（单一 dominant） | ~110 | 理论与实测一致 |
| 多值正确映射 | ~13 | 多 SQLCOM 分布符合语法逻辑 |
| 假 dominant（explain_stmt） | 1 | dominant 为噪音，覆盖本身正确 |
| RSG blacklist 导致未捕获 | 9 | 可手工注入解决 |
| Patch 不触发（GET_DIAG/SIGNAL） | 2 | 需修改 patch 插桩位置 |
| C++ 层赋值但未捕获 | 5 | 可手工注入解决 |
| 完全不可达（EMPTY_QUERY） | 1 | 架构限制，可忽略 |

### 修复

1. **手工注入 SQL**（高优先，低成本）：为 `SQLCOM_ALTER_USER_DEFAULT_ROLE`、`SQLCOM_GRANT_ROLE`、`SQLCOM_REVOKE_ROLE`、`SQLCOM_SET_OPTION`、`SQLCOM_SET_PASSWORD`、`SQLCOM_ALTER_DB`、`SQLCOM_ALTER_USER`、`SQLCOM_SET_ROLE`、`SQLCOM_CREATE_SPFUNCTION` 添加硬编码 seed queries，可将覆盖率从 88.7% 提升到约 95%。

2. **修复 explain_stmt dominant**：将 `explain_stmt` 的 `dominant_sqlcom` 手工修正为 `SQLCOM_EXPLAIN_OTHER`，或在生成 queries 时加一个 `EXPLAIN SELECT 1` 类型的 seed。

3. **patch 插桩位置调整**（低优先，高成本）：将 mysqld patch 从 `mysql_execute_command()` 入口改为更早期位置，以捕获 `SIGNAL`/`GET DIAGNOSTICS`。

4. **SQLCOM_EMPTY_QUERY**：无需处理，空查询在 fuzzer 中实际价值为零。
