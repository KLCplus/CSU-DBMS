"""CSUDB 2026 Native JSON 协议的轻量 DB-API 2.0 风格驱动。

核心原理：本模块不包含 SQL 引擎，只做协议翻译——把 Python 调用编码成
Native 协议请求（utf-8 JSON 文本 + 一个 NUL 结束符），经 TCP 发往服务端，
再把响应的 UTF-8 JSON 翻译成 PEP 249 异常与客户端物化的结果集。
所有传输细节都封装在 Connection 与 Cursor 之内，使用方无需接触套接字。
实现只依赖 Python 标准库，适合教学工具、脚本、本地 Web Console 与框架适配。

Small DB-API style driver for the CSUDB 2026 Native JSON protocol.

The wire format is intentionally hidden behind Connection and Cursor.  This
module uses only Python's standard library and is suitable for teaching tools,
scripts, the local Web Console, and future framework adapters.
"""

from __future__ import annotations

import json
import socket
import threading
from typing import Any, Iterable, Optional, Sequence

# PEP 249 要求模块暴露的三个属性：接口版本、线程安全级别、参数占位符风格。
# 这里声明 threadsafety=1 表示线程之间不能共享连接，但可以各自持有连接。

apilevel = "2.0"
threadsafety = 1
paramstyle = "format"

# 单次响应体积上限，与服务端和其他客户端保持一致

_MAX_PACKET = 16 * 1024 * 1024


# PEP 249 规定的异常层级：Warning 与 Error 两支，
# Error 之下按错误性质细分，DatabaseError 再派生数据、操作、完整性等子类。
# 使用方可以按需捕获，例如只处理 OperationalError 表示网络与连接问题。

# 告警基类：PEP 249 规定用于非致命提示，当前实现不主动抛出，仅供使用方捕获。
class Warning(Exception):
    pass


# 所有错误异常的根，独立于 Warning 分支。
class Error(Exception):
    pass


# 接口层错误：驱动自身使用不当（如对已关闭连接发起请求、响应格式非法）。
class InterfaceError(Error):
    pass


# 数据库错误基类：携带服务端返回的错误信息。
# 参数 message 为人类可读描述；code 是服务端错误码（缺省 1）；name 是错误名。
# 原理：把 super().__init__ 的消息交给 Exception 保存，另存 code/name 两个属性，
# 使使用方既能 str() 取消息，也能按错误码分类处理。
class DatabaseError(Error):
    def __init__(self, message: str, code: int = 1, name: str = "") -> None:
        super().__init__(message)
        self.code = code
        self.name = name


# 数据错误：数值越界、类型不匹配等由数据本身引发的问题。
class DataError(DatabaseError):
    pass


# 操作错误：连接断开、认证失败等网络与运行环境问题（不应归咎于 SQL 本身）。
class OperationalError(DatabaseError):
    pass


# 完整性错误：主键/唯一约束、外键等约束被违反。
class IntegrityError(DatabaseError):
    pass


# 内部错误：服务端内部故障，通常意味着服务端状态损坏。
class InternalError(DatabaseError):
    pass


# 编程错误：SQL 语法错、表不存在、占位符与参数个数不匹配等调用方错误。
class ProgrammingError(DatabaseError):
    pass


# 不支持错误：驱动或服务端不支持请求的能力。
class NotSupportedError(DatabaseError):
    pass


# 把一个 Python 值转成 SQL 字面量。
# 参数 value：待转换的任意 Python 值；返回：可直接拼进 SQL 文本的字面量字符串。
# 实现原理：驱动端做转义拼接，而不是服务端参数化查询——
# None 转 NULL、布尔转 0/1、bytes 转无引号十六进制串，
# int/float 直接 str()，其余一律当作字符串并把内部单引号加倍（''）。

def _quote(value: Any) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, bytes):
        return "'" + value.hex() + "'"
    return "'" + str(value).replace("'", "''") + "'"


# 把 SQL 里的 %s 占位符按顺序替换成转义后的字面量。
# 参数 operation：带 %s 的 SQL 模板；parameters：与占位符一一对应的参数序列，
# None 表示不绑定；返回：替换完成的 SQL 文本。
# 实现原理：按 "%s" 切分模板，若片段数减一与参数个数不等则抛 ProgrammingError，
# 避免参数错位悄悄拼出语义错误的语句；随后逐段用 _quote 的结果与后缀重新拼接。

def _bind(operation: str, parameters: Optional[Sequence[Any]]) -> str:
    if parameters is None:
        return operation
    parts = operation.split("%s")
    if len(parts) - 1 != len(parameters):
        raise ProgrammingError("placeholder count does not match parameters")
    result = [parts[0]]
    for value, suffix in zip(parameters, parts[1:]):
        result.extend((_quote(value), suffix))
    return "".join(result)


# 传输层：一条 TCP 连接加上请求响应配对。
# 协议是「UTF-8 JSON 加一个 NUL 结尾」，用锁串行化同一连接上的收发。
# 原理：连接对象被 Connection 独占，不做连接池；锁保证同一时刻只有一个请求在途，
# 从而不会出现响应错配。

class _NativeConnection:
    # 建立连接并设置超时。
    # 参数 host/port：服务端地址；timeout：连接与读取超时秒数。
    # 返回：无（构造器）；失败时把 OSError 转成 OperationalError，
    # 便于使用方区分网络问题与协议/数据问题。
    def __init__(self, host: str, port: int, timeout: float) -> None:
        try:
            self.socket = socket.create_connection((host, port), timeout=timeout)
        except OSError as exc:
            raise OperationalError(f"cannot connect to CSUDB at {host}:{port}: {exc}") from exc
        self.socket.settimeout(timeout)
        self.lock = threading.Lock()

    # 关闭套接字。
    # 参数：无；返回：无。实现上先把 self.socket 置 None（使 closed 判定生效），
    # 再关闭旧套接字；因此可重复调用而不会出错。
    def close(self) -> None:
        sock, self.socket = self.socket, None
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass

    # 发一次请求并读回响应。
    # 参数 payload：要序列化的请求字典；返回：解析后的响应字典。
    # 实现原理：先用紧凑 JSON 序列化并补一个 b"\0" 结束符，sendall 保证整包写完；
    # 接收时以 4096 字节为单位累积，遇到 NUL 即认为一帧结束，
    # 并以 16 MiB（_MAX_PACKET）为上限防止异常响应把内存撑爆。
    # 网络错误转 OperationalError，帧格式非法转 InterfaceError。
    def request(self, payload: dict[str, Any]) -> dict[str, Any]:
        if self.socket is None:
            raise InterfaceError("connection is closed")
        packet = json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\0"
        with self.lock:
            try:
                self.socket.sendall(packet)
                chunks = bytearray()
                while len(chunks) <= _MAX_PACKET:
                    block = self.socket.recv(4096)
                    if not block:
                        raise OperationalError("CSUDB closed the connection")
                    marker = block.find(b"\0")
                    chunks.extend(block if marker < 0 else block[:marker])
                    if marker >= 0:
                        break
                else:
                    raise OperationalError("CSUDB response exceeds safety limit")
            except OSError as exc:
                raise OperationalError(f"CSUDB network error: {exc}") from exc
        try:
            response = json.loads(chunks.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise InterfaceError("invalid response from CSUDB") from exc
        if not isinstance(response, dict):
            raise InterfaceError("invalid response type from CSUDB")
        return response


class Connection:
    """A CSUDB session.

    Connections are not shared between threads. Cursor values are materialized
    by the server response, so fetch methods do not keep database internals.
    """

    # 建立连接后立即用 login 请求完成认证。
    # 参数 host/port：服务端地址；user/password：账号口令；database：初始库；
    # timeout：超时秒数。返回：无（构造器）。
    # 实现原理：先建 _NativeConnection，再发 type=login 请求；认证失败时
    # 必须关掉已建立的套接字再抛错，否则会留下已连接但未登录的会话。
    # 成功后把服务端 attributes 存为 server_attributes，并以服务端确认的库名覆盖 database。
    def __init__(
        self,
        host: str,
        port: int,
        user: str,
        password: str,
        database: str,
        timeout: float,
    ) -> None:
        self.host = host
        self.port = port
        self.user = user
        self.database = database
        self.autocommit = True
        self._native = _NativeConnection(host, port, timeout)
        response = self._native.request(
            {"type": "login", "user": user, "password": password, "database": database}
        )
        try:
            _raise_for_error(response)
        except Exception:
            self._native.close()
            raise
        self.server_attributes = response.get("attributes") or {}
        self.database = self.server_attributes.get("database", database)

    # 连接是否已关闭。
    # 参数：无；返回 bool。原理：以底层套接字是否已释放（置 None）作为唯一判据。
    @property
    def closed(self) -> bool:
        return self._native.socket is None

    # 创建一个游标。
    # 参数：无；返回绑定到本连接的 Cursor。若连接已关闭则抛 InterfaceError。
    def cursor(self) -> "Cursor":
        if self.closed:
            raise InterfaceError("connection is closed")
        return Cursor(self)

    # 便捷方法，等价于 cursor().execute()。
    # 参数 operation：SQL 模板；parameters：参数序列；返回执行后的 Cursor。
    def execute(self, operation: str, parameters: Optional[Sequence[Any]] = None) -> "Cursor":
        return self.cursor().execute(operation, parameters)

    # 提交事务。
    # 参数：无；返回：无。原理：当前服务端的提交只是一条普通 SQL，故直接执行 COMMIT;。
    def commit(self) -> None:
        self.execute("COMMIT;")

    # 回滚事务。
    # 参数：无；返回：无。原理：同 commit，回滚也只是执行一条 ROLLBACK; 语句。
    def rollback(self) -> None:
    # 回滚事务
        self.execute("ROLLBACK;")

    # 诊断扩展：取服务端信息。
    # 参数：无；返回服务端信息字典。走服务端 server_info 请求类型。
    def server_info(self) -> dict[str, Any]:
        return self._request({"type": "server_info"})

    # 诊断扩展：取 Buffer Pool 快照。
    # 参数 limit：页数上限，缺省 20，返回：快照字典。
    # 原理：把 limit 夹在 0 到 200 之间后再发给服务端，防止拉取过量数据。
    def buffer_snapshot(self, limit: int = 20) -> dict[str, Any]:
        return self._request({"type": "buffer_snapshot", "limit": max(0, min(int(limit), 200))})

    # 统一的请求出口。
    # 参数 payload：请求字典；返回：服务端响应字典。
    # 原理：先发请求，再统一用 _raise_for_error 把失败响应转成异常；
    # 若响应 attributes 带回当前数据库名，则同步更新本地 database 状态。
    def _request(self, payload: dict[str, Any]) -> dict[str, Any]:
        response = self._native.request(payload)
        _raise_for_error(response)
        attributes = response.get("attributes") or {}
        if "database" in attributes:
            self.database = attributes["database"]
        return response

    # 关闭连接。
    # 参数：无；返回：无。已关闭则直接返回（幂等）。
    # 原理：先尽力发 logout 让服务端回收会话，忽略其异常，
    # 最后在 finally 中释放套接字，确保无论 logout 成败连接都被关闭。
    def close(self) -> None:
        if self.closed:
            return
        try:
            self._native.request({"type": "logout"})
        except Error:
            pass
        finally:
            self._native.close()

    # 支持 with 语句。
    # 参数：无；返回自身。原理：__enter__ 不做额外动作，真正的清理在 __exit__。
    def __enter__(self) -> "Connection":
        return self

    # with 块结束时的清理。
    # 参数：异常三元组（可全为 None）；返回 None。
    # 原理：无论块内是否抛异常都调用 close，避免连接泄漏。
    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        self.close()


# 游标：负责绑定参数、发起查询，并在客户端物化结果集。
# 原理：一次 execute 就把所有行取回并存成字符串元组列表，fetch* 只移动本地下标、
# 不再访问网络，因此游标不持有任何服务端游标或数据库内部结构。
class Cursor:
    arraysize = 1

    # 初始化游标。
    # 参数 connection：所属 Connection；返回：无（构造器）。
    # 原理：description/rowcount 初始为「未知」状态，_rows 为空、_position 为 0，
    # 真正的数据在 execute 时才由响应填充。
    def __init__(self, connection: Connection) -> None:
        self.connection = connection
        self.description: Optional[list[tuple[Any, ...]]] = None
        self.rowcount = -1
        self.last_result: dict[str, Any] = {}
        self._rows: list[tuple[str, ...]] = []
        self._position = 0
        self._closed = False

    # 执行一条语句。
    # 参数 operation：SQL 模板；parameters：参数序列（可空）；返回执行后的 self。
    # 原理：先用 _bind 把 %s 参数绑定成转义后的字面量，再发 type=query 请求；
    # 随后根据响应生成 description（列元数据）与 rowcount（有列时为行数，
    # 无列时取 affected_rows，缺失则 -1）；行数据在这里就全部转成字符串元组，
    # 因此后续 fetch 不再访问网络。已关闭的游标抛 InterfaceError。
    def execute(
        self, operation: str, parameters: Optional[Sequence[Any]] = None
    ) -> "Cursor":
        if self._closed:
            raise InterfaceError("cursor is closed")
        sql = _bind(operation, parameters)
        self.last_result = self.connection._request({"type": "query", "sql": sql})
        columns = self.last_result.get("columns") or []
        self.description = [
            (column.get("name", ""), column.get("type", ""), None, None, None, None, None)
            for column in columns
        ] or None
        self._rows = [tuple(str(cell) for cell in row) for row in self.last_result.get("rows") or []]
        self._position = 0
        affected = self.last_result.get("affected_rows")
        self.rowcount = len(self._rows) if columns else (-1 if affected is None else int(affected))
        return self

    # 逐组参数重复执行。
    # 参数 operation：SQL 模板；seq_of_parameters：多组参数；返回执行后的 self。
    # 原理：对每组参数调用一次 execute，把各自的影响行数累加作为最终 rowcount；
    # 只有 rowcount 非负（即服务端给出了影响行数）时才计入总和。
    def executemany(self, operation: str, seq_of_parameters: Iterable[Sequence[Any]]) -> "Cursor":
        total = 0
        for parameters in seq_of_parameters:
            self.execute(operation, parameters)
            if self.rowcount >= 0:
                total += self.rowcount
        self.rowcount = total
        return self

    # 取下一行。
    # 参数：无；返回：下一行字符串元组，已取完则返回 None。
    # 原理：仅推进本地 _position 下标，不触发网络请求。
    def fetchone(self) -> Optional[tuple[str, ...]]:
        if self._position >= len(self._rows):
            return None
        row = self._rows[self._position]
        self._position += 1
        return row

    # 取若干行。
    # 参数 size：期望行数，None 时用 self.arraysize；返回：行元组列表。
    # 原理：从 _position 起切片，最多取 size 行，并同步推进 _position；
    # 负数 size 会被归零，因此不会出现负向切片。
    def fetchmany(self, size: Optional[int] = None) -> list[tuple[str, ...]]:
        count = self.arraysize if size is None else max(0, int(size))
        rows = self._rows[self._position : self._position + count]
        self._position += len(rows)
        return rows

    # 取剩余全部行。
    # 参数：无；返回：从当前位置到末尾的所有行。
    # 原理：切片后把 _position 移到末尾，使后续 fetch 返回空。
    def fetchall(self) -> list[tuple[str, ...]]:
        rows = self._rows[self._position :]
        self._position = len(self._rows)
        return rows

    # 关闭游标并释放已物化的行数据。
    # 参数：无；返回：无。原理：置 _closed 标记阻止后续 execute，并清空 _rows 释放内存。
    def close(self) -> None:
        self._closed = True
        self._rows = []

    # 支持 for row in cursor 迭代。
    # 参数：无；返回：自身作为迭代器。
    def __iter__(self) -> "Cursor":
        return self

    # 迭代取行。
    # 参数：无；返回：下一行。原理：复用 fetchone，取完时抛 StopIteration 结束迭代。
    def __next__(self) -> tuple[str, ...]:
        row = self.fetchone()
        if row is None:
            raise StopIteration
        return row


# 把失败响应翻译成异常。
# 参数 response：服务端响应字典；返回：无（成功时直接返回）。
# 实现原理：服务端统一返回 success 字段与 error 对象；这里取出 message、code、name
# 填入 DatabaseError 实例后抛出（name 仅作为属性保存，不据此派生子类）。
# 错误消息按 error.message、response.message、固定兜底文案的顺序回退。

def _raise_for_error(response: dict[str, Any]) -> None:
    if response.get("success", False):
        return
    error = response.get("error") or {}
    raise DatabaseError(
        error.get("message") or response.get("message") or "CSUDB request failed",
        int(error.get("code") or 1),
        str(error.get("name") or ""),
    )


# 模块级连接入口。
# 参数：host/port 服务端地址，user/password 账号口令，database 初始库，
# timeout 超时秒数，**kwargs 用于捕获未知选项。
# 返回：已登录的 Connection。原理：先拒绝任何未知关键字参数，避免拼错的选项
# 被静默忽略；再对端口做 1..65535 范围校验；其余参数在构造 Connection 时完成类型转换。

def connect(
    host: str = "127.0.0.1",
    port: int = 6789,
    user: str = "root",
    password: str = "",
    database: str = "sys",
    timeout: float = 5.0,
    **kwargs: Any,
) -> Connection:
    """Open an authenticated CSUDB Native-protocol session."""

    if kwargs:
        unknown = ", ".join(sorted(kwargs))
        raise TypeError(f"unsupported connection options: {unknown}")
    if not 0 < int(port) <= 65535:
        raise InterfaceError("port must be between 1 and 65535")
    return Connection(host, int(port), user, password, database, float(timeout))
