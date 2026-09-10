# CSUDB 2026 项目演示手册

本文是一份可以直接照着操作和讲解的现场演示脚本，建议总时长为 8～10 分钟。演示内容覆盖产品外壳、SQL 编译执行主线、数据库核心功能和 OS Page/Buffer Pool 实现。

## 1. 演示目标

向老师展示 CSUDB 已经形成下面这条完整链路：

```text
csudb CLI
  -> csudbd / DatabaseService / Session
  -> Lexer / Parser / Resolver
  -> Logical Plan / Optimizer / Physical Plan
  -> Executor
  -> Table / Record / RID
  -> DiskBufferPool / Frame / Page
  -> PageIOBackend
  -> Linux file I/O / disk
```

一句话介绍：

> CSUDB 2026 是在 MiniOB 成熟内核基础上整理和扩展的课程数据库，将编译原理、数据库系统和操作系统三条主线连接到同一个可安装、可登录、可持久化的数据库产品中。

## 2. 演示前检查

打开两个终端，并确认全局命令可用：

```bash
csudbd --version
csudb --version
```

预期均显示：

```text
CSUDB 2026 2026.1.0
```

确认默认数据位置：

```bash
csudbd --help | tail -n 2
```

正常默认目录为：

```text
~/.local/state/csudb
```

如端口已被占用，检查是否已经启动过服务端：

```bash
ss -ltn | grep 6789
```

不要在正式演示前删除数据目录。初始化只执行一次。

## 3. 第一次初始化（约 1 分钟）

当前机器如果还没有初始化，在终端一执行：

```bash
csudbd --initialize
```

按照提示输入自己准备的演示密码：

```text
Choose the password for the initial root account.
Enter root password:
Confirm root password:
```

说明要点：

- 密码输入不回显；
- 密码至少 8 位；
- 两次输入必须一致；
- Catalog 中保存 PBKDF2-HMAC-SHA256 哈希和随机 Salt，不保存明文；
- 系统不会生成无法找回的随机初始密码；
- 普通启动不会隐式创建另一套 Catalog。

成功标志：

```text
System catalog : created
Root user      : created
Root password  : configured by user
Initialization complete.
```

如果已经初始化，跳过本节，绝对不要再次执行 `--initialize`。

## 4. 启动服务端（约 30 秒）

终端一执行：

```bash
csudbd \
  --buffer-size 262144 \
  --replacement clock \
  --io-backend positional
```

说明要点：

- 默认只监听 `127.0.0.1:6789`；
- `262144 / 8192 = 32`，本次演示的 Buffer Pool 大约有 32 个 Frame；
- Page 固定为 8 KiB，这是现有磁盘格式的一部分；
- `clock` 是 Second Chance 页面替换策略；
- `positional` 后端使用可靠循环的 `pread/pwrite`；
- 还可选择 `lru`、`fifo` 和 `legacy`。

保持终端一运行。

## 5. 登录产品 CLI（约 1 分钟）

终端二执行：

```bash
csudb
```

输入初始化时设置的 Root 密码。登录后展示 CSUDB 2026 Banner 和提示符：

```text
csudb [sys]>
```

输入：

```text
/
```

然后按 Tab 展示命令候选。再演示前缀补全：

```text
/sta<Tab>
```

应补全或提示 `/status`。演示输错保护：

```text
/statsu
```

应看到相近命令建议：

```text
Did you mean /status?
```

说明要点：Meta Command 在客户端处理，不进入 SQL Parser；旧反斜杠形式仅作为兼容别名保留。

## 6. 数据库与表演示（约 3 分钟）

以下语句逐条执行。SQL 必须以分号结束。

### 6.1 创建并切换数据库

```sql
CREATE DATABASE school;
SHOW DATABASES;
USE school;
```

也可以展示客户端命令：

```text
/database
```

提示符应变为：

```text
csudb [school]>
```

### 6.2 创建表

```sql
CREATE TABLE student (
    id INT,
    name CHAR(32),
    age INT
);
```

```sql
SHOW TABLES;
DESC student;
```

说明要点：`CREATE TABLE` 经过 Parser、Resolver 和 CommandExecutor，最终由 `Db::create_table`、`Table::create` 和 Buffer Pool 创建持久化文件。

### 6.3 插入记录

```sql
INSERT INTO student VALUES (1, 'Alice', 20);
INSERT INTO student VALUES (2, 'Bob', 21);
INSERT INTO student VALUES (3, 'Carol', 19);
```

说明要点：记录经过序列化后，由 RecordPageHandler 分配 slot；RID 使用 `(page_num, slot_num)` 定位记录。

### 6.4 查询和条件过滤

```sql
SELECT * FROM student;
```

预期包含三行：Alice、Bob、Carol。

```sql
SELECT name, age
FROM student
WHERE id = 1;
```

预期只返回 Alice。

说明要点：可以沿着 `Parser -> Resolve -> Logical Plan -> Optimizer -> Physical Operator -> Table Scan -> RecordPage` 讲解完整执行链。

### 6.5 删除并验证

```sql
DELETE FROM student
WHERE id = 1;
```

```sql
SELECT * FROM student;
```

预期只剩 Bob 和 Carol。

## 7. OS / Buffer Pool 演示（约 2 分钟）

在客户端执行：

```text
/status
/buffer
/pages 10
```

重点观察：

```text
Page Size
Buffer Pool Size / Capacity
Replacement Policy
I/O Backend
Requests
Hits / Misses
Disk Reads / Writes
Pinned Frames
Dirty Frames
```

讲解关系：

```text
Record 的 RID(page, slot)
        -> RecordPage
        -> DiskBufferPool::get_this_page
        -> Frame 被 pin
        -> 命中时直接使用内存 Page
        -> 未命中时 PageIOBackend 从文件读取 8192 字节
        -> 修改后标记 dirty
        -> unpin 后才可能被 CLOCK 选为 victim
        -> 脏页在 Flush/淘汰/关闭时写回磁盘
```

对应 OS 真实源码入口：

```text
src/observer/storage/os/page/
src/observer/storage/os/buffer/
src/observer/storage/os/replacement/
src/observer/storage/os/io/
src/observer/storage/os/diagnostics/
```

## 8. 持久化演示（约 1 分钟）

客户端退出：

```text
/quit
```

回到服务端终端按 `Ctrl+C`，等待正常退出。不要使用 `kill -9`。

重新启动，参数保持一致：

```bash
csudbd \
  --buffer-size 262144 \
  --replacement clock \
  --io-backend positional
```

另一个终端重新登录：

```bash
csudb
```

验证：

```sql
USE school;
SELECT * FROM student;
```

预期重启后仍能看到 Bob 和 Carol。由此证明用户、Catalog、表元数据和记录均已持久化。

## 9. 可选：用户和权限演示

时间充足时再演示，不建议挤占 SQL/OS 主线时间。

Root 会话中执行：

```sql
CREATE USER 'alice' IDENTIFIED BY 'AlicePass2026!';
GRANT SELECT ON school.student TO 'alice';
SHOW GRANTS FOR 'alice';
```

退出后以 Alice 登录：

```bash
csudb -u alice -p -D school
```

查询应成功：

```sql
SELECT * FROM student;
```

未授权的写操作应被拒绝：

```sql
DELETE FROM student WHERE id = 2;
```

说明要点：认证和权限统一经过 DatabaseService、Session 和 SystemCatalog，不在 Executor 中散落用户名判断。

## 10. 源码展示顺序

如果老师要求看源码，建议按以下顺序打开，避免现场在目录中寻找：

1. `src/observer/net/sql_task_handler.cpp`：统一 SQL 请求入口；
2. `src/observer/sql/parser/parse_stage.cpp`：Parser 阶段；
3. `src/observer/sql/parser/resolve_stage.cpp`：语义解析；
4. `src/observer/sql/optimizer/logical_plan_generator.cpp`：逻辑计划；
5. `src/observer/sql/optimizer/optimize_stage.cpp`：优化阶段；
6. `src/observer/sql/executor/execute_stage.cpp`：执行入口；
7. `src/observer/storage/record/record_manager.cpp`：Record/RID/Page 映射；
8. `src/observer/storage/os/buffer/disk_buffer_pool.cpp`：Buffer Pool；
9. `src/observer/storage/os/replacement/replacement_policy.cpp`：LRU/FIFO/CLOCK；
10. `src/observer/storage/os/io/page_io_backend.cpp`：磁盘 I/O 后端。

## 11. 常见现场问题

### 提示未初始化

```text
ERROR: CSUDB data directory is not initialized.
```

仅在确实是新机器时执行：

```bash
csudbd --initialize
```

### 连接失败

```bash
csudb --ping
```

若失败，确认终端一的 `csudbd` 仍在运行，且端口为 6789。

### 认证失败

确认输入的是初始化时自己设置的密码。密码输入不显示字符是正常现象。

### 数据库或表已存在

说明上一次演示数据仍在。不要现场删除整个数据目录，可以改用新名字：

```sql
CREATE DATABASE school_demo;
USE school_demo;
```

### 端口被占用

不要强制杀死未知进程。可用独立端口启动：

```bash
csudbd --port 16789
```

客户端对应连接：

```bash
csudb -P 16789
```

## 12. 30 秒总结词

> 本项目没有重新实现第二套数据库，而是保留 MiniOB 的成熟架构，并完成 CSUDB 产品化和课程化整理。编译原理部分负责把 SQL 转换为语义正确的计划；数据库部分负责优化、执行、Catalog、Record、索引和事务；操作系统部分以 8 KiB Page、Frame、Buffer Pool、替换策略和文件 I/O 为核心。现在系统可以通过统一 CLI 登录、执行 SQL、查看 Buffer Pool 状态，并在重启后保持数据。后续可在现有接口上继续增加 AST/Plan 可视化、Page Trace、B+Tree 可视化、WAL Recovery 和 MVCC 实验。
