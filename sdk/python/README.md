# CSUDB Python 驱动 · 接口文档

`csudb.py` 是 CSUDB 2026 Native JSON 协议的轻量 Python 驱动，遵循 DB-API 2.0 风格。
仅依赖 Python 标准库，适用于课程实验、自动化脚本、Web/API 适配器等场景。

- 模块级常量：`apilevel = "2.0"`、`threadsafety = 1`、`paramstyle = "format"`。
- 协议细节（JSON 报文、长度前缀等）全部封装在 `Connection` / `Cursor` 之内。

## 1. 安装与导入

从源码或发行包中获取 `csudb.py`（安装树中位于 `share/csudb/sdk/python/csudb.py`）：

```bash
# 发行包方式
tar -xzf csudb-python-sdk-2026.1.0.tar.gz
export PYTHONPATH="$PWD/csudb-python-sdk-2026.1.0:$PYTHONPATH"
```

```python
import csudb
```

## 2. 快速开始

```python
import csudb

with csudb.connect(
    host="127.0.0.1",
    port=6789,
    user="root",
    password="csudb1234",
    database="sys",
) as db:
    cur = db.execute("SELECT id, name FROM student WHERE age >= %s;", (20,))
    print([column[0] for column in cur.description])
    for row in cur:
        print(row)
```

## 3. `connect(...)`

打开一个已认证的 CSUDB 会话并返回 `Connection`。

```python
csudb.connect(host="127.0.0.1", port=6789, user="root",
              password="", database="sys", timeout=5.0)
```

| 参数 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `host` | `str` | `"127.0.0.1"` | 服务端地址，可为域名或公网 IP |
| `port` | `int` | `6789` | native 协议端口，取值 1–65535 |
| `user` | `str` | `"root"` | 账号 |
| `password` | `str` | `""` | 密码 |
| `database` | `str` | `"sys"` | 初始数据库，登录后可被服务端返回值覆盖 |
| `timeout` | `float` | `5.0` | 连接与读写超时（秒） |

- 传入其它关键字参数会抛出 `TypeError`；端口非法会抛出 `InterfaceError`。

## 4. `Connection`

一个会话。**不可跨线程共享**（`threadsafety = 1`）。

| 成员 | 说明 |
| --- | --- |
| `host` / `port` / `user` / `database` | 连接信息；`database` 会随 `USE` 等服务端返回更新 |
| `server_attributes` | 登录响应中的服务端属性（`dict`） |
| `autocommit` | 当前恒为 `True` |
| `closed` | 连接是否已关闭（只读属性） |
| `cursor()` | 新建一个 `Cursor` |
| `execute(operation, parameters=None)` | 便利方法，等价于 `cursor().execute(...)`，返回 `Cursor` |
| `commit()` | 执行 `COMMIT;` |
| `rollback()` | 执行 `ROLLBACK;` |
| `server_info()` | 服务与 Buffer Pool 摘要（`dict`） |
| `buffer_snapshot(limit=20)` | 只读 Page/Frame 快照；`limit` 会被限制在 0–200 |
| `close()` | 发送 `logout` 并关闭底层 socket（可重复调用） |
| `with` 语句 | `__enter__` 返回自身，`__exit__` 自动 `close()` |

## 5. `Cursor`

物化结果集游标。服务端一次响应即返回完整结果，`fetch*` 不再访问数据库内部。

| 成员 | 说明 |
| --- | --- |
| `execute(operation, parameters=None)` | 执行一条 SQL，返回自身 |
| `executemany(operation, seq_of_parameters)` | 依次对每组参数执行，`rowcount` 为累计值 |
| `fetchone()` | 返回下一行 `tuple[str, ...]`，无更多行返回 `None` |
| `fetchmany(size=None)` | 返回最多 `size` 行（默认 `arraysize`） |
| `fetchall()` | 返回剩余所有行 |
| `description` | 列元信息列表，元素为 `(name, type, None, None, None, None, None)`；非查询为 `None` |
| `rowcount` | 查询为行数；DML 为 `affected_rows`；未知为 `-1` |
| `last_result` | 最近一次响应的原始 `dict` |
| `arraysize` | `fetchmany` 默认批量，类属性，默认 `1` |
| `close()` | 关闭游标（本地） |
| 迭代 | 支持 `for row in cursor:`，内部调用 `fetchone()` |

## 6. 占位符与类型

- 占位符为 `%s`，按出现顺序与参数序列一一对应；数量不符抛 `ProgrammingError`。
- 使用前值会按 SQL 字面量规则转义：`None→NULL`、`bool→1/0`、数字原样、`bytes→十六进制字符串`、其余加单引号并转义 `'`。
- 这是**客户端转义**，不是服务端 PreparedStatement。
- 返回单元格当前统一为字符串（`str`），类型转换由应用完成。

## 7. 异常

异常层级（均继承 `Exception`）：

```text
Warning
Error
├── InterfaceError
└── DatabaseError        # 含属性 .code、.name
    ├── DataError
    ├── OperationalError
    ├── IntegrityError
    ├── InternalError
    ├── ProgrammingError
    └── NotSupportedError
```

`DatabaseError.code` / `.name` 来自服务端错误响应。

## 8. 公网/远程示例

```python
import csudb

with csudb.connect(
    host="117.50.163.43", port=8157,
    user="root", password="csudb1234",
    database="sys", timeout=10,
) as db:
    cur = db.execute("SELECT id, name FROM student WHERE age >= %s;", (20,))
    for row in cur:
        print(row)
```

## 9. 限制

- 一次响应物化完整结果，不适合超大结果集。
- 非服务端 PreparedStatement。
- 返回单元格当前统一为字符串。
- `threadsafety = 1`，不要跨线程共享 `Connection`。
- TLS 与连接池尚未实现。
