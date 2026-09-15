"""Small DB-API style driver for the CSUDB 2026 Native JSON protocol.

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

class Warning(Exception):
    pass


class Error(Exception):
    pass


class InterfaceError(Error):
    pass


class DatabaseError(Error):
    def __init__(self, message: str, code: int = 1, name: str = "") -> None:
        super().__init__(message)
        self.code = code
        self.name = name


class DataError(DatabaseError):
    pass


class OperationalError(DatabaseError):
    pass


class IntegrityError(DatabaseError):
    pass


class InternalError(DatabaseError):
    pass


class ProgrammingError(DatabaseError):
    pass


class NotSupportedError(DatabaseError):
    pass


# 把一个 Python 值转成 SQL 字面量。
# 注意这是驱动端的转义拼接，不是服务端的参数化查询：
# None 转 NULL、布尔转 0/1、bytes 转十六进制串，其余字符串把单引号加倍。

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
# 占位符个数与参数个数必须一致，否则直接报错，避免参数错位拼出错误语句。

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

class _NativeConnection:
    # 建立连接并设置超时；失败时转成 OperationalError，便于使用方区分网络问题
    def __init__(self, host: str, port: int, timeout: float) -> None:
        try:
            self.socket = socket.create_connection((host, port), timeout=timeout)
        except OSError as exc:
            raise OperationalError(f"cannot connect to CSUDB at {host}:{port}: {exc}") from exc
        self.socket.settimeout(timeout)
        self.lock = threading.Lock()

    # 关闭套接字，可重复调用
    def close(self) -> None:
        sock, self.socket = self.socket, None
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass

    # 发一次请求并读回响应。
    # 发送用 sendall 保证整个包写完；接收循环累积到遇到 NUL 为止，
    # 并以 16 MiB 为上限防止异常响应把内存撑爆。
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

    # 建立连接后立即用 login 请求完成认证；认证失败要关掉套接字再抛错，
    # 否则会留下一个已连接但未登录的会话。
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

    # 套接字已释放即视为已关闭
    @property
    def closed(self) -> bool:
        return self._native.socket is None

    # 创建一个游标
    def cursor(self) -> "Cursor":
        if self.closed:
            raise InterfaceError("connection is closed")
        return Cursor(self)

    # 便捷方法，等价于 cursor().execute()
    def execute(self, operation: str, parameters: Optional[Sequence[Any]] = None) -> "Cursor":
        return self.cursor().execute(operation, parameters)

    # 提交事务。当前服务端的提交与回滚都只是一条普通 SQL
    def commit(self) -> None:
        self.execute("COMMIT;")

    def rollback(self) -> None:
    # 回滚事务
        self.execute("ROLLBACK;")

    # 诊断扩展：取服务端信息
    def server_info(self) -> dict[str, Any]:
        return self._request({"type": "server_info"})

    # 诊断扩展：取 Buffer Pool 快照，上限被夹在 0 到 200 之间
    def buffer_snapshot(self, limit: int = 20) -> dict[str, Any]:
        return self._request({"type": "buffer_snapshot", "limit": max(0, min(int(limit), 200))})

    # 统一的请求出口：出错就抛异常，并在响应带回当前数据库时同步本地状态
    def _request(self, payload: dict[str, Any]) -> dict[str, Any]:
        response = self._native.request(payload)
        _raise_for_error(response)
        attributes = response.get("attributes") or {}
        if "database" in attributes:
            self.database = attributes["database"]
        return response

    # 关闭连接：先尝试 logout，无论成败都要释放套接字
    def close(self) -> None:
        if self.closed:
            return
        try:
            self._native.request({"type": "logout"})
        except Error:
            pass
        finally:
            self._native.close()

    # 支持 with 语句，退出时自动关闭
    def __enter__(self) -> "Connection":
        return self

    # with 块结束时的清理，异常与否都关闭连接
    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        self.close()


class Cursor:
    arraysize = 1

    # 结果集全部在客户端物化，游标本身不持有任何数据库内部结构
    def __init__(self, connection: Connection) -> None:
        self.connection = connection
        self.description: Optional[list[tuple[Any, ...]]] = None
        self.rowcount = -1
        self.last_result: dict[str, Any] = {}
        self._rows: list[tuple[str, ...]] = []
        self._position = 0
        self._closed = False

    # 执行一条语句。
    # 先把参数绑定成字面量，再发 query 请求；随后根据响应生成
    # description（列元数据）与 rowcount（有列时为行数，无列时取影响行数）。
    # 行数据在这里就全部转成字符串元组，后续 fetch 不再访问网络。
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

    # 逐组参数重复执行，rowcount 是各组影响行数之和
    def executemany(self, operation: str, seq_of_parameters: Iterable[Sequence[Any]]) -> "Cursor":
        total = 0
        for parameters in seq_of_parameters:
            self.execute(operation, parameters)
            if self.rowcount >= 0:
                total += self.rowcount
        self.rowcount = total
        return self

    # 取下一行，取完返回 None
    def fetchone(self) -> Optional[tuple[str, ...]]:
        if self._position >= len(self._rows):
            return None
        row = self._rows[self._position]
        self._position += 1
        return row

    # 取若干行，默认用 arraysize
    def fetchmany(self, size: Optional[int] = None) -> list[tuple[str, ...]]:
        count = self.arraysize if size is None else max(0, int(size))
        rows = self._rows[self._position : self._position + count]
        self._position += len(rows)
        return rows

    # 取剩余全部行
    def fetchall(self) -> list[tuple[str, ...]]:
        rows = self._rows[self._position :]
        self._position = len(self._rows)
        return rows

    # 关闭游标并释放已物化的行数据
    def close(self) -> None:
        self._closed = True
        self._rows = []

    # 支持 for row in cursor 迭代
    def __iter__(self) -> "Cursor":
        return self

    # 迭代取行，取完抛 StopIteration
    def __next__(self) -> tuple[str, ...]:
        row = self.fetchone()
        if row is None:
            raise StopIteration
        return row


# 把失败响应翻译成异常。
# 服务端统一返回 success 字段与 error 对象，这里取出消息、错误码与错误名，
# 填进 DatabaseError 的子类实例。

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
# 拒绝任何未知关键字参数，避免拼错的选项被静默忽略；
# 端口做范围校验，其余参数在构造 Connection 时完成类型转换。

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
