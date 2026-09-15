#!/usr/bin/env python3
"""Local-only HTTP gateway for the CSUDB 2026 Web Console."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import secrets
import signal
import sys
import threading
from http import HTTPStatus
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# 复用仓库内的 Python SDK 作为唯一的 Native 协议客户端实现，
# 这样 Web 不需要自己重写一遍协议编解码；安装后 SDK 不在源码树里时退回只加当前目录。

SCRIPT_DIR = Path(__file__).resolve().parent
SOURCE_SDK = SCRIPT_DIR.parents[2] / "sdk" / "python" if len(SCRIPT_DIR.parents) > 2 else None
if SOURCE_SDK and SOURCE_SDK.is_dir():
    sys.path.insert(0, str(SOURCE_SDK))
sys.path.insert(0, str(SCRIPT_DIR))

import csudb  # noqa: E402

# 请求体上限 1 MiB，避免超大请求占用内存
# 标识符白名单：表名与库名会被拼进 SQL，必须先用这个正则校验，防止 SQL 注入
MAX_BODY = 1024 * 1024
IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


# 会话表：把随机 token 映射到已登录的数据库连接
# 浏览器只保存 token，密码只在登录那一次用掉，之后不落在任何地方
class SessionStore:
    def __init__(self) -> None:
        self._sessions: dict[str, csudb.Connection] = {}
        self._lock = threading.Lock()

    # 生成一个密码学安全的随机 token 并绑定连接。用 secrets 而不是 random，
    # 因为前者来自操作系统的安全随机源，不可预测。
    def create(self, connection: csudb.Connection) -> str:
        token = secrets.token_urlsafe(32)
        with self._lock:
            self._sessions[token] = connection
        return token

    # 按 token 取回连接，取不到返回 None
    def get(self, token: str) -> csudb.Connection | None:
        with self._lock:
            return self._sessions.get(token)

    # 注销：移出会话表并关闭对应的数据库连接
    def remove(self, token: str) -> None:
        with self._lock:
            connection = self._sessions.pop(token, None)
        if connection:
            connection.close()

    # 进程退出时关闭全部残留连接
    def close_all(self) -> None:
        with self._lock:
            connections = list(self._sessions.values())
            self._sessions.clear()
        for connection in connections:
            connection.close()


# 支持并发的 HTTP 服务：每个请求一个线程，且随主进程一起退出

class WebConsoleServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    # 绑定监听地址，并记住要连接的数据库服务端地址

    def __init__(self, address: tuple[str, int], handler: type[BaseHTTPRequestHandler], db_host: str, db_port: int):
        super().__init__(address, handler)
        self.db_host = db_host
        self.db_port = db_port
        self.sessions = SessionStore()


# 只挑选可以对外暴露的字段，避免把服务端内部结构原样透传

def public_result(response: dict) -> dict:
    return {
        "success": response.get("success", False),
        "error": response.get("error") or {},
        "message": response.get("message", ""),
        "columns": response.get("columns") or [],
        "rows": response.get("rows") or [],
        "affected_rows": response.get("affected_rows"),
        "execution_time_us": response.get("execution_time_us", 0),
        "warnings": response.get("warnings") or [],
        "attributes": response.get("attributes") or {},
    }


# HTTP 请求处理器：静态资源走 GET，数据接口按路径分发

class Handler(BaseHTTPRequestHandler):
    server_version = "CSUDB-Web/2026.1"

    # 统一日志前缀，便于在 web.log 里筛选

    def log_message(self, message: str, *args: object) -> None:
        sys.stdout.write("[CSUDB_WEB] " + message % args + "\n")
        sys.stdout.flush()

    # 安全响应头：禁止浏览器猜内容类型、禁止被其他站点嵌进 iframe、
    # 不记录来源、限制资源加载范围、不缓存任何响应。
    # CORS 允许外部来源加载的前端调用本接口，鉴权走自定义请求头而不是 Cookie。
    def _security_headers(self) -> None:
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; style-src 'self' 'unsafe-inline'; "
            "script-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'",
        )
        self.send_header("Cache-Control", "no-store")
        # 允许由 GitHub Pages 等外部来源加载的前端跨域调用本 API（使用 X-CSUDB-Session 头，不依赖 Cookie）
        origin = self.headers.get("Origin")
        self.send_header("Access-Control-Allow-Origin", origin if origin else "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-CSUDB-Session")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Vary", "Origin")

    # 预检请求直接返回 204
    def do_OPTIONS(self) -> None:
        self.send_response(HTTPStatus.NO_CONTENT)
        self._security_headers()
        self.end_headers()

    # 发送 JSON 响应，可选同时下发会话 Cookie
    def _send_json(self, status: int, value: dict, cookie: str | None = None) -> None:
        body = json.dumps(value, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self._security_headers()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        if cookie is not None:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        self.wfile.write(body)

    # 发送静态文件
    def _send_file(self, path: Path, content_type: str) -> None:
        try:
            body = path.read_bytes()
        except OSError:
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        self.send_response(HTTPStatus.OK)
        self._security_headers()
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # 读取并解析 JSON 请求体，同时校验类型、长度与体积上限
    def _read_json(self) -> dict:
        content_type = self.headers.get("Content-Type", "")
        if not content_type.startswith("application/json"):
            raise ValueError("Content-Type must be application/json")
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > MAX_BODY:
            raise ValueError("invalid request size")
        value = json.loads(self.rfile.read(length).decode("utf-8"))
        if not isinstance(value, dict):
            raise ValueError("JSON body must be an object")
        return value

    # 取会话 token：自定义请求头优先，其次 Cookie。
    # 两种方式是为了适配内嵌预览等 Cookie 受限的场景。
    def _session_token(self) -> str:
        # 自定义请求头优先（适配嵌入式预览/iframe 等 Cookie 受限场景），其次 Cookie
        header_token = self.headers.get("X-CSUDB-Session", "").strip()
        if header_token:
            return header_token
        cookie = SimpleCookie(self.headers.get("Cookie", ""))
        value = cookie.get("csudb_session")
        return "" if value is None else value.value

    def _connection(self) -> csudb.Connection | None:
        return self.server.sessions.get(self._session_token())

    # 要求已登录，未登录直接返回 401
    def _require_connection(self) -> csudb.Connection | None:
        connection = self._connection()
        if connection is None:
            self._send_json(HTTPStatus.UNAUTHORIZED, {"success": False, "error": {"message": "login required"}})
        return connection

    # GET 接口：静态页面、健康检查，以及状态、页快照、库表清单、表数据与结构

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/":
            self._send_file(SCRIPT_DIR / "index.html", "text/html; charset=utf-8")
            return
        if parsed.path == "/app.css":
            self._send_file(SCRIPT_DIR / "app.css", "text/css; charset=utf-8")
            return
        if parsed.path == "/app.js":
            self._send_file(SCRIPT_DIR / "app.js", "text/javascript; charset=utf-8")
            return
        if parsed.path == "/api/health":
            self._send_json(HTTPStatus.OK, {
                "success": True,
                "product": "CSUDB 2026",
                "database_endpoint": f"{self.server.db_host}:{self.server.db_port}",
            })
            return
        connection = self._require_connection()
        if connection is None:
            return
        try:
            if parsed.path == "/api/status":
                status = connection.server_info()
                pages = connection.buffer_snapshot(80)
                self._send_json(HTTPStatus.OK, {"success": True, "status": public_result(status), "pages": public_result(pages)})
            elif parsed.path == "/api/pages":
                query = parse_qs(parsed.query)
                limit = max(1, min(int(query.get("limit", ["40"])[0]), 200))
                self._send_json(HTTPStatus.OK, public_result(connection.buffer_snapshot(limit)))
            elif parsed.path == "/api/databases":
                self._send_json(HTTPStatus.OK, public_result(connection._request({"type": "query", "sql": "SHOW DATABASES;"})))
            elif parsed.path == "/api/tables":
                self._send_json(HTTPStatus.OK, public_result(connection._request({"type": "query", "sql": "SHOW TABLES;"})))
            elif parsed.path == "/api/table":
                name = parse_qs(parsed.query).get("name", [""])[0]
                if not IDENTIFIER.fullmatch(name):
                    raise ValueError("invalid table name")
                result = public_result(connection._request({"type": "query", "sql": f"SELECT * FROM {name};"}))
                result["rows"] = result["rows"][:100]
                result["limited_to"] = 100
                result["server_pagination"] = False
                self._send_json(HTTPStatus.OK, result)
            elif parsed.path == "/api/schema":
                name = parse_qs(parsed.query).get("name", [""])[0]
                if not IDENTIFIER.fullmatch(name):
                    raise ValueError("invalid table name")
                description = public_result(connection._request({"type": "query", "sql": f"DESC {name};"}))
                self._send_json(HTTPStatus.OK, {
                    "success": True,
                    "table": name,
                    "columns": [{"name": row[0], "type": row[1], "length": row[2]}
                                for row in description["rows"] if len(row) >= 3],
                    "indexes": [],
                    "foreign_keys": [],
                    "index_metadata_available": False,
                    "foreign_key_metadata_available": False,
                })
            else:
                self._send_json(HTTPStatus.NOT_FOUND, {"success": False, "error": {"message": "unknown endpoint"}})
        except (csudb.Error, ValueError) as exc:
            self._send_json(HTTPStatus.BAD_REQUEST, {"success": False, "error": {"message": str(exc)}})

    # POST 接口：登录、注销、执行 SQL、切换数据库、SQL 补全

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        try:
            body = self._read_json()
        except (ValueError, json.JSONDecodeError) as exc:
            self._send_json(HTTPStatus.BAD_REQUEST, {"success": False, "error": {"message": str(exc)}})
            return

        if parsed.path == "/api/connect":
            try:
                connection = csudb.connect(
                    host=self.server.db_host,
                    port=self.server.db_port,
                    user="root",
                    password=str(body.get("password", "")),
                    database="sys",
                )
            except csudb.Error as exc:
                self._send_json(HTTPStatus.UNAUTHORIZED, {"success": False, "error": {"message": "authentication failed"}})
                return
            token = self.server.sessions.create(connection)
            cookie = f"csudb_session={token}; Path=/; HttpOnly; SameSite=Lax"
            self._send_json(HTTPStatus.OK, {
                "success": True,
                "session_token": token,
                "attributes": connection.server_attributes,
                "database": connection.database,
            }, cookie)
            return

        token = self._session_token()
        connection = self._require_connection()
        if connection is None:
            return
        try:
            if parsed.path == "/api/logout":
                self.server.sessions.remove(token)
                self._send_json(HTTPStatus.OK, {"success": True}, "csudb_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax")
            elif parsed.path == "/api/query":
                sql = str(body.get("sql", "")).strip()
                if not sql:
                    raise ValueError("SQL is empty")
                self._send_json(HTTPStatus.OK, public_result(connection._request({"type": "query", "sql": sql})))
            elif parsed.path == "/api/use":
                database = str(body.get("database", ""))
                if not IDENTIFIER.fullmatch(database):
                    raise ValueError("invalid database name")
                result = connection._request({"type": "query", "sql": f"USE {database};"})
                self._send_json(HTTPStatus.OK, public_result(result))
            elif parsed.path == "/api/complete":
                sql = str(body.get("sql", ""))
                cursor = body.get("cursor", len(sql))
                try:
                    cursor = max(0, min(int(cursor), len(sql)))
                except (TypeError, ValueError):
                    cursor = len(sql)
                result = connection._request({
                    "type": "complete",
                    "sql": sql,
                    "cursor": cursor,
                    "max_items": 12,
                    "want_model": bool(body.get("want_model", False)),
                })
                self._send_json(HTTPStatus.OK, public_result(result))
            else:
                self._send_json(HTTPStatus.NOT_FOUND, {"success": False, "error": {"message": "unknown endpoint"}})
        except (csudb.Error, ValueError) as exc:
            self._send_json(HTTPStatus.BAD_REQUEST, {"success": False, "error": {"message": str(exc)}})


# 命令行参数：数据库地址、Web 监听地址与 pid 文件位置

def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="CSUDB 2026 local Web Console")
    parser.add_argument("--db-host", default="127.0.0.1")
    parser.add_argument("--db-port", type=int, default=6789)
    parser.add_argument("--listen-host", default="127.0.0.1")
    parser.add_argument("--listen-port", type=int, default=8765)
    parser.add_argument("--pid-file")
    return parser.parse_args()


# 入口：强制只监听本地回环地址，写 pid 文件，注册优雅退出

def main() -> int:
    args = parse_arguments()
    if args.listen_host not in {"127.0.0.1", "::1", "localhost"}:
        print("ERROR: CSUDB Web Console is local-only; listen on 127.0.0.1", file=sys.stderr)
        return 2
    if not 0 < args.db_port <= 65535 or not 0 < args.listen_port <= 65535:
        print("ERROR: invalid port", file=sys.stderr)
        return 2

    server = WebConsoleServer((args.listen_host, args.listen_port), Handler, args.db_host, args.db_port)
    pid_file = Path(args.pid_file) if args.pid_file else None
    if pid_file:
        pid_file.parent.mkdir(parents=True, exist_ok=True)
        pid_file.write_text(str(os.getpid()), encoding="ascii")
        os.chmod(pid_file, 0o600)

    # 收到终止信号时在另一个线程里关闭服务，避免阻塞信号处理函数

    def shutdown(signum: int, frame: object) -> None:
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    print(
        f"CSUDB Web Console listening at http://{args.listen_host}:{args.listen_port} "
        f"for database {args.db_host}:{args.db_port}",
        flush=True,
    )
    try:
        server.serve_forever(poll_interval=0.25)
    finally:
        server.sessions.close_all()
        server.server_close()
        if pid_file:
            try:
                pid_file.unlink()
            except FileNotFoundError:
                pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
