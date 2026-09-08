# CSU-DBMS CLI 使用指南

## 1. 快速开始

在仓库根目录完成 Debug 构建，然后直接启动：

```bash
./build.sh debug --make -j4
./csudb
```

`./csudb` 会自动选择 Debug 构建产物、加载 `etc/csudb.ini` 并进入本地交互模式，不需要先启动服务器或手工填写 `-P cli`。

## 2. 进入界面

正常启动后显示 CSU-DBMS 欢迎页和以下提示符：

```text
csudb >
```

常用交互：

- `help;`：查看 SQL 示例。
- `show tables;`：查看表。
- `desc student;`：查看表结构。
- `exit`、`bye`、`\q`：退出。
- 上下方向键：浏览历史命令。

建议 SQL 以分号结尾；当前 Shell 按一次输入提交一条语句。

## 3. 第一次 SQL 会话

```sql
CREATE TABLE student (id INT, name CHAR(20));
INSERT INTO student VALUES (1, 'Alice');
INSERT INTO student VALUES (2, 'Bob');
SELECT * FROM student;
SELECT name FROM student WHERE id = 1;
DELETE FROM student WHERE id = 1;
SELECT * FROM student;
```

退出后再次执行 `./csudb`，运行 `SELECT * FROM student;`，可以验证数据持久化。

## 4. 命令行参数

```bash
./csudb --help
./csudb --version
./csudb --buffer-size 262144 --replacement fifo
./csudb --transaction mvcc --durable
```

常用参数：

| 参数 | 作用 |
| --- | --- |
| `--config PATH` | 指定配置文件 |
| `--buffer-size BYTES` | 设置 Buffer Pool 容量 |
| `--replacement lru\|fifo` | 选择页替换策略 |
| `--transaction vacuous\|mvcc` | 选择事务模型 |
| `--durable` | 启用磁盘日志持久化模式 |
| `--engine heap\|lsm` | 选择存储引擎 |

## 5. 网络模式

终端一启动服务：

```bash
./csudb server --port 6789
```

终端二连接：

```bash
build_debug/bin/csudb-client -h 127.0.0.1 -p 6789
```

兼容可执行文件 `observer` 和 `obclient` 暂时保留，方便旧测试与脚本继续运行；新开发和演示统一使用 `csudb`、`csudb-client`。

## 6. 文件位置

| 路径 | 内容 |
| --- | --- |
| `csudb_data/db/sys/` | 默认数据库和表文件 |
| `.csudb_history` | 本地 Shell 历史 |
| `.csudb_client_history` | 网络客户端历史 |
| `csudb.log.<日期>` | 运行、Buffer Pool Trace 和错误日志 |
| `etc/csudb.ini` | 默认配置 |

数据目录相对于启动时的工作目录。需要隔离实验数据时，在独立目录中直接运行仓库内的绝对路径 `build_debug/bin/csudb`。

## 7. 常见问题

- 提示找不到 `build_debug/bin/csudb`：先执行 `./build.sh debug --make -j4`。
- 配置加载失败：优先从仓库根目录使用 `./csudb`；包装脚本会自动传入正确配置。
- 建表提示表已存在：数据是持久化的，可更换表名，或在确认不再需要数据后清理对应实验运行目录。
- 需要观察缓存行为：使用 `--replacement fifo`/`lru` 和较小的 `--buffer-size`，再查看 `csudb.log.<日期>`。
