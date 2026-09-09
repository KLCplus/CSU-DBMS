# CSUDB 2026 使用与维护手册

本文只说明如何使用 CSUDB。第一次按“首次初始化”操作，以后每天只需要“启动服务端”和“进入客户端”两步。

## 1. 最常用的三个命令

进入项目目录：

```bash
cd /home/konglingchen/code/DBMS/miniob
```

终端一启动数据库服务：

```bash
./build_debug/bin/csudbd \
  --config ./etc/csudb.ini \
  --data-dir ./csudb_data
```

终端二进入数据库：

```bash
./build_debug/bin/csudb -u root -p
```

停止：客户端输入 `\q`，然后在服务端终端按 `Ctrl+C`。

> `Enter password:` 后输入密码时屏幕不会显示字符或星号。正常输入完密码后按 Enter 即可。

## 2. 第一次使用

### 2.1 编译

在项目根目录执行：

```bash
./build.sh debug --make -j4
```

成功后应存在：

```text
build_debug/bin/csudb
build_debug/bin/csudbd
```

### 2.2 初始化

初始化只执行一次。把下面的 `MyRootPass2026!` 换成自己的密码，至少 8 个字符：

```bash
env CSUDB_INITIAL_ROOT_PASSWORD='MyRootPass2026!' \
  ./build_debug/bin/csudbd \
  --initialize \
  --config ./etc/csudb.ini \
  --data-dir ./csudb_data
```

看到以下内容表示成功：

```text
System catalog : created
Root user      : created
Initialization complete.
```

初始化以后不要再次运行 `--initialize`。Root 密码会持久化，重启不会失效。

## 3. 日常启动与登录

### 3.1 启动服务端

打开第一个终端：

```bash
cd /home/konglingchen/code/DBMS/miniob

./build_debug/bin/csudbd \
  --config ./etc/csudb.ini \
  --data-dir ./csudb_data
```

默认监听：

```text
Host : 127.0.0.1
Port : 6789
```

这个终端需要保持运行。

### 3.2 进入客户端

打开第二个终端：

```bash
cd /home/konglingchen/code/DBMS/miniob
./build_debug/bin/csudb -u root -p
```

成功后显示欢迎页和提示符：

```text
csudb [sys]>
```

检查服务是否在线：

```bash
./build_debug/bin/csudb --ping
```

正常输出：

```text
CSUDB server is alive.
```

## 4. 第一个数据库

进入客户端后依次执行：

```sql
CREATE DATABASE school;
USE school;

CREATE TABLE student (
  id INT,
  name CHAR(32),
  age INT
);

INSERT INTO student VALUES (1, 'Alice', 20);
INSERT INTO student VALUES (2, 'Bob', 21);

SELECT * FROM student;
SELECT name FROM student WHERE id = 1;

DELETE FROM student WHERE id = 1;
SELECT * FROM student;
```

查看数据库和表：

```sql
SHOW DATABASES;
SHOW TABLES;
DESC student;
```

## 5. 客户端常用命令

反斜杠开头的是客户端命令，不是 SQL：

| 命令 | 用途 |
| --- | --- |
| `\help` | 查看 Shell 帮助 |
| `\q` | 退出客户端 |
| `\status` | 查看连接、Session、Page Size 和 Buffer Pool 状态 |
| `\database` | 查看当前数据库 |
| `\use school` | 切换到 school |
| `\timing on` | 显示 SQL 执行时间 |
| `\history` | 查看本次会话历史 |
| `\source demo.sql` | 执行 SQL 文件 |
| `\output result.txt` | 将结果输出到文件 |
| `\output` | 恢复输出到屏幕 |
| `\buffer` | 查看 Buffer Pool 摘要 |
| `\pages 20` | 查看前 20 个 Frame/Page 快照 |
| `\server` | 查看服务端状态 |

SQL 可以分多行输入，遇到字符串外的 `;` 或 `\g` 才执行：

```text
csudb [school]> SELECT id,
    -> name
    -> FROM student
    -> WHERE age > 18;
```

## 6. 用户与权限

Root 创建用户：

```sql
CREATE USER 'alice'
IDENTIFIED BY 'AlicePass2026!';
```

允许 Alice 查询 `school.student`：

```sql
GRANT SELECT
ON school.student
TO 'alice';
```

查看权限：

```sql
SHOW GRANTS FOR 'alice';
```

退出 Root：

```text
\q
```

使用 Alice 登录：

```bash
./build_debug/bin/csudb \
  -u alice \
  -p \
  -D school
```

修改密码：

```sql
ALTER USER 'alice'
IDENTIFIED BY 'NewAlicePass2026!';
```

收回权限：

```sql
REVOKE SELECT
ON school.student
FROM 'alice';
```

删除用户：

```sql
DROP USER 'alice';
```

## 7. 执行一条 SQL 或 SQL 文件

执行一条 SQL 后退出：

```bash
./build_debug/bin/csudb \
  -u root \
  -p \
  -D school \
  -e "SELECT * FROM student;"
```

执行 SQL 文件：

```bash
./build_debug/bin/csudb \
  -u root \
  -p \
  -D school \
  -f demo.sql
```

脚本友好的制表符输出：

```bash
./build_debug/bin/csudb \
  -u root \
  -p \
  -D school \
  --batch \
  -e "SELECT * FROM student;"
```

SQL 失败时客户端返回非零退出码，可用于脚本或 CI。

## 8. 正确停止

客户端退出：

```text
\q
```

回到服务端终端，按：

```text
Ctrl+C
```

这样服务端会走正常关闭和刷新流程。不要把 `kill -9` 当作正常停止方式。

## 9. 数据保存在哪里

本文命令统一使用：

```text
/home/konglingchen/code/DBMS/miniob/csudb_data
```

相对仓库根目录就是：

```text
./csudb_data
```

主要内容：

```text
csudb_data/
├── system/catalog.json       用户、数据库和权限目录
└── db/
    ├── sys/                  系统默认数据库
    └── school/               用户数据库
        ├── student.table     表元数据
        └── student.data      表记录页
```

初始化与以后启动必须使用同一个 `--data-dir`。在其他目录启动时，建议使用绝对路径：

```bash
--data-dir /home/konglingchen/code/DBMS/miniob/csudb_data
```

## 10. 备份与恢复

当前版本没有在线热备份工具。课程和开发环境推荐冷备份：

1. 在客户端输入 `\q`。
2. 在服务端终端按 `Ctrl+C`。
3. 确认 `csudbd` 已停止。
4. 复制整个 `csudb_data` 目录。

示例：

```bash
cp -a ./csudb_data ./csudb_data.backup
```

恢复时同样先停止服务，然后将完整备份目录作为新的 `--data-dir`：

```bash
./build_debug/bin/csudbd \
  --config ./etc/csudb.ini \
  --data-dir ./csudb_data.backup
```

不要只复制某一个 `.data` 文件；Catalog、表元数据、索引和日志需要保持为同一份一致快照。

## 11. OS / Buffer Pool 实验

这些参数设置在服务端，不设置在客户端。

LRU + legacy：

```bash
./build_debug/bin/csudbd \
  --config ./etc/csudb.ini \
  --data-dir ./csudb_data \
  --replacement lru \
  --io-backend legacy
```

FIFO + positional：

```bash
./build_debug/bin/csudbd \
  --config ./etc/csudb.ini \
  --data-dir ./csudb_data \
  --replacement fifo \
  --io-backend positional
```

CLOCK + positional，并设置 32 个 8 KiB Frame：

```bash
./build_debug/bin/csudbd \
  --config ./etc/csudb.ini \
  --data-dir ./csudb_data \
  --buffer-size 262144 \
  --replacement clock \
  --io-backend positional
```

进入客户端后查看：

```text
\status
\buffer
\pages 20
```

## 12. 安装成全局命令

安装到当前用户：

```bash
cd /home/konglingchen/code/DBMS/miniob
cmake --install build_debug --prefix "$HOME/.local"
```

确认 PATH 包含：

```bash
export PATH="$HOME/.local/bin:$PATH"
```

之后可以在任意目录运行：

```bash
csudbd --help
csudb --help
csudb --ping
```

启动服务端时仍建议明确指定数据目录：

```bash
csudbd \
  --config /home/konglingchen/code/DBMS/miniob/etc/csudb.ini \
  --data-dir /home/konglingchen/code/DBMS/miniob/csudb_data
```

卸载程序：

```bash
cd /home/konglingchen/code/DBMS/miniob
./scripts/uninstall.sh --user
```

卸载脚本不会删除 `csudb_data`。

## 13. 常见问题

### 连接被拒绝

```text
ERROR: cannot connect to CSUDB server at 127.0.0.1:6789
```

先检查：

```bash
./build_debug/bin/csudb --ping
```

若失败，说明服务端未启动、已经退出或端口不一致。回到终端一启动 `csudbd`。

### 输入密码时没有任何显示

这是正常安全行为。继续输入完整密码并按 Enter。

### 重启后找不到用户或数据库

通常是启动时换了 `--data-dir`。确保初始化和每次启动都使用：

```text
/home/konglingchen/code/DBMS/miniob/csudb_data
```

### 提示认证失败

确认用户名和密码一致：

```bash
./build_debug/bin/csudb -u root -p
```

密码不能从 Catalog 中恢复，因为只保存安全派生值。如果仍有 Root 会话，可执行：

```sql
ALTER USER 'root' IDENTIFIED BY 'NewRootPass2026!';
```

当前版本尚未提供离线 Root 密码恢复命令。不要通过删除 data-dir 处理忘记密码，否则会丢失现有数据。

### 提示数据库已经初始化

不要再次执行 `--initialize`。直接使用日常启动命令。

### 表已经存在

说明数据已持久化。使用 `SHOW TABLES;` 和 `DESC table;` 查看，不要重复建表。

### 哪些 SQL 目前不能用

当前稳定核心是 CREATE TABLE、INSERT、SELECT、WHERE 和 DELETE。UPDATE、DROP TABLE、DROP INDEX、ORDER BY、JOIN keyword、OR 和 unary NOT 尚未作为正式支持能力。完整审计见 `COMMAND_AUDIT.md`。

## 14. 相关文档

- `docs/product/COMMANDS.md`：完整命令参考。
- `docs/product/COMMAND_AUDIT.md`：SQL 支持情况审计。
- `docs/product/ARCHITECTURE.md`：客户端、服务端和内核边界。
- `docs/course/os_storage.md`：Page、Buffer Pool 与 I/O 实验。
