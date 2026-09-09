# CSUDB 2026 CLI 使用指南

CSUDB 是客户端/服务端数据库。`csudbd` 负责 SQL、Session、Catalog、Executor 和 Storage；`csudb` 只负责连接、认证、输入与结果展示，不在客户端重复解析 SQL。

## 1. 构建与初始化

```bash
./build.sh debug --make -j4

build_debug/bin/csudbd --initialize \
  --config etc/csudb.ini
```

初始化只执行一次。服务端会关闭密码回显，要求输入并确认至少 8 位的 Root 密码。密码不会明文写入 Catalog。

## 2. 启动 Server

```bash
build_debug/bin/csudbd \
  --config etc/csudb.ini \
  --host 127.0.0.1 \
  --port 6789
```

服务端默认绑定 `127.0.0.1`，默认协议是需要登录的 CSUDB native protocol。默认数据固定在 `~/.local/state/csudb`（或 `$XDG_STATE_HOME/csudb`），因此从任何目录执行 `csudbd` 都连接同一套数据。按 Ctrl+C 触发优雅退出。缓存实验参数也在服务端设置：

```bash
build_debug/bin/csudbd --data-dir /tmp/csudb-course \
  --buffer-size 262144 --replacement clock --io-backend positional
```

## 3. 连接 Client

```bash
build_debug/bin/csudb -h 127.0.0.1 -P 6789 -u root -p
```

密码无回显。连接成功后提示符是 `csudb [sys]>`；选择数据库后显示相应名称。输入可跨行，只有字符串外的分号或 `\g` 才提交。

```text
csudb [school]> SELECT id,
    -> name
    -> FROM student
    -> WHERE id = 1;
```

## 4. 第一次会话

```sql
CREATE DATABASE school;
USE school;
CREATE TABLE student (id INT, name CHAR(32), age INT);
INSERT INTO student VALUES (1, 'Alice', 20);
INSERT INTO student VALUES (2, 'Bob', 21);
SELECT * FROM student;
DELETE FROM student WHERE id = 1;
SELECT * FROM student;
```

重新启动服务端并再次查询，可以验证 Database、Table、Record 和用户目录的持久化。

## 5. Shell 常用功能

- `/help`：显示全部已实现 Meta Command。
- `/status`、`/server`：查看真实服务端/Session 配置。
- `/buffer`、`/pages 20`：读取稳定 Buffer Pool Snapshot DTO。
- `/timing on`：显示耗时。
- `/source init.sql`、`/output result.txt`：执行脚本和重定向。
- `/quit`：退出。

在空输入行键入 `/` 并按 Tab 可列出候选；输入命令前缀可补全，拼写错误时会显示相近命令。旧的 `\` 命令仅作为兼容别名保留。

批处理与脚本：

```bash
csudb -u root -p school -e "SELECT * FROM student;"
csudb -u root -p -D school -f report.sql --batch
csudb -u root -p school < report.sql
csudb --ping
```

历史保存在 `~/.csudb/history`。包含 `IDENTIFIED BY` 或 `PASSWORD` 的输入不会写入历史。Profile 文件位于 `~/.csudb/config.toml`，可参考 `etc/csudb-client.toml`；配置文件不支持保存密码。

## 6. 安装

```bash
cmake --install build_debug --prefix "$HOME/.local"
# 或
CSUDB_BUILD_DIR="$PWD/build_debug" ./scripts/install.sh --user
```

把 `~/.local/bin` 加入 PATH 后，可在任意目录执行 `csudb` 和 `csudbd`。卸载执行 `./scripts/uninstall.sh --user`；脚本不会删除数据库 data-dir。

参数、SQL 能力边界与 Planned 项目以 `docs/product/COMMANDS.md` 为准。
