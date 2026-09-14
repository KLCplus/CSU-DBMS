# CSUDB 2026 · SQL 客户端（Linux x86_64）

CSUDB 数据库的**命令行客户端**（native 协议）。解压即用，无需安装、无需服务端源码。

## 1. 环境要求

- Linux x86_64
- glibc ≥ 2.35（在 Ubuntu 22.04 上构建；更老的发行版可能不兼容）
- 客户端只依赖系统 `libc`/`libm`，`libstdc++` 已静态链接

## 2. 快速开始

```bash
tar -xzf csudb-client-linux-x64.tar.gz
cd csudb-client-linux-x64
chmod +x csudb

# 连接线上演示服务（公网）
./csudb --url 117.50.163.43:8157 -u root -p
# 密码：csudb1234
```

进入交互界面后输入 SQL，以 `;` 结尾执行；输入 `/help` 查看命令，`/q` 退出。
输入时支持 **Tab 补全**（关键字 / 表 / 列）与 **AI ghost 提示**。

## 3. 选择本地或线上（`--url`）

客户端支持用 `--url` 指定服务地址（`host[:port]`，可带 `native://` / `http(s)://` 前缀），
也可用短选项 `-U` 或环境变量 `CSUDB_URL`：

```bash
# 线上（公网演示服务）
./csudb --url 117.50.163.43:8157 -u root -p
# 本地
./csudb --url 127.0.0.1:6789 -u root -p
# 等价写法
./csudb -U 117.50.163.43:8157 -u root -p
CSUDB_URL=117.50.163.43:8157 ./csudb -u root -p
```

优先级：显式 `-h/-P` > `--url`/`CSUDB_URL` > 配置文件/环境变量。

## 4. 常用命令

```bash
# 连接测试（不需要密码）
./csudb --url 117.50.163.43:8157 --ping

# 执行单条 SQL 后退出
./csudb --url 117.50.163.43:8157 -u root -p -e "SELECT * FROM student;"

# 执行 SQL 文件后退出
./csudb --url 117.50.163.43:8157 -u root -p -f script.sql

# 查看补全候选（无需进入交互）
./csudb --url 117.50.163.43:8157 -u root --complete "SELECT * FR"

# 交互命令（部分）
#   /help        帮助
#   /status      服务/会话/缓冲池状态
#   /use DB      切换数据库
#   /timing on   显示耗时
#   /complete SQL 查看补全候选
#   /q           退出
```

密码也可以通过环境变量提供，便于脚本：

```bash
CSUDB_PASSWORD=csudb1234 ./csudb --url 117.50.163.43:8157 -u root -e "SHOW TABLES;"
```

## 5. 选项速查

```
-h, --host HOST    服务地址（默认 127.0.0.1）
-P, --port PORT    端口（默认 6789）
-U, --url URL      服务地址 host[:port]，可选本地或线上
-u, --user USER    用户名（默认 root）
-p, --password     交互式输入密码
-D, --database DB  初始数据库
-e, --execute SQL  执行单条 SQL 后退出
-f, --file FILE    执行 SQL 文件后退出
    --complete SQL 打印补全候选后退出
    --ping         探测连通性
    --batch        制表符脚本输出
    --help         帮助
```

## 6. 安全提示

- 线上演示服务为公网可访问，密码为演示用；请勿在其中存放敏感数据。
- 服务端由所有者维护，若不可访问请确认地址与端口。

## 7. 版权

本客户端包含基于 OceanBase Miniob 改造的代码，版权与许可说明见随包的 `NOTICE`。
