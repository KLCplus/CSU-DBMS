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

apilevel = "2.0"
threadsafety = 1
paramstyle = "format"

_MAX_PACKET = 16 * 1024 * 1024


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


class _NativeConnection:
    def __init__(self, host: str, port: int, timeout: float) -> None:
        try:
            self.socket = socket.create_connection((host, port), timeout=timeout)
        except OSError as exc:
            raise OperationalError(f"cannot connect to CSUDB at {host}:{port}: {exc}") from exc
        self.socket.settimeout(timeout)
        self.lock = threading.Lock()

    def close(self) -> None:
        sock, self.socket = self.socket, None
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass

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

    @property
    def closed(self) -> bool:
        return self._native.socket is None

    def cursor(self) -> "Cursor":
        if self.closed:
            raise InterfaceError("connection is closed")
        return Cursor(self)

    def execute(self, operation: str, parameters: Optional[Sequence[Any]] = None) -> "Cursor":
        return self.cursor().execute(operation, parameters)

    def commit(self) -> None:
        self.execute("COMMIT;")

    def rollback(self) -> None:
        self.execute("ROLLBACK;")

    def server_info(self) -> dict[str, Any]:
        return self._request({"type": "server_info"})

    def buffer_snapshot(self, limit: int = 20) -> dict[str, Any]:
        return self._request({"type": "buffer_snapshot", "limit": max(0, min(int(limit), 200))})

    def _request(self, payload: dict[str, Any]) -> dict[str, Any]:
        response = self._native.request(payload)
        _raise_for_error(response)
        attributes = response.get("attributes") or {}
        if "database" in attributes:
            self.database = attributes["database"]
        return response

    def close(self) -> None:
        if self.closed:
            return
        try:
            self._native.request({"type": "logout"})
        except Error:
            pass
        finally:
            self._native.close()

    def __enter__(self) -> "Connection":
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        self.close()


class Cursor:
    arraysize = 1

    def __init__(self, connection: Connection) -> None:
        self.connection = connection
        self.description: Optional[list[tuple[Any, ...]]] = None
        self.rowcount = -1
        self.last_result: dict[str, Any] = {}
        self._rows: list[tuple[str, ...]] = []
        self._position = 0
        self._closed = False

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

    def executemany(self, operation: str, seq_of_parameters: Iterable[Sequence[Any]]) -> "Cursor":
        total = 0
        for parameters in seq_of_parameters:
            self.execute(operation, parameters)
            if self.rowcount >= 0:
                total += self.rowcount
        self.rowcount = total
        return self

    def fetchone(self) -> Optional[tuple[str, ...]]:
        if self._position >= len(self._rows):
            return None
        row = self._rows[self._position]
        self._position += 1
        return row

    def fetchmany(self, size: Optional[int] = None) -> list[tuple[str, ...]]:
        count = self.arraysize if size is None else max(0, int(size))
        rows = self._rows[self._position : self._position + count]
        self._position += len(rows)
        return rows

    def fetchall(self) -> list[tuple[str, ...]]:
        rows = self._rows[self._position :]
        self._position = len(self._rows)
        return rows

    def close(self) -> None:
        self._closed = True
        self._rows = []

    def __iter__(self) -> "Cursor":
        return self

    def __next__(self) -> tuple[str, ...]:
        row = self.fetchone()
        if row is None:
            raise StopIteration
        return row


def _raise_for_error(response: dict[str, Any]) -> None:
    if response.get("success", False):
        return
    error = response.get("error") or {}
    raise DatabaseError(
        error.get("message") or response.get("message") or "CSUDB request failed",
        int(error.get("code") or 1),
        str(error.get("name") or ""),
    )


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
