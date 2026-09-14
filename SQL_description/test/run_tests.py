#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CSUDB SQL 编译器 + 基础 SQL 功能测试集

覆盖 requirements.md 中「必须要完成的验收能力」与词法/语法/语义分析的关键要求：
  - 必备 SQL：CREATE / INSERT / SELECT / DELETE / WHERE / 比较运算 / AND / OR / NOT / 括号
  - 词法：注释、多字符运算符、字符串与转义、大小写、非法输入
  - 语法：优先级、括号、语法诊断（位置 + unexpected + expected 集合）
  - 语义：表/列存在性、名字绑定、类型一致性、INSERT 匹配、错误定位
  - 边界：空输入、极长标识符、多语句、大小写、无分号
  - 指标统计：Crash / Wrong Accept / Wrong Reject / Wrong Location

默认会自行拉起一个使用临时数据目录的 csudbd 服务端，保证可重复运行；
也可以 --host/--port 连接已启动的服务端。

用法：
    python3 SQL_description/test/run_tests.py
    python3 SQL_description/test/run_tests.py --host 127.0.0.1 --port 6789 --password xxx
    python3 SQL_description/test/run_tests.py --filter lexer --verbose
"""

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent  # <repo>/SQL_description/test -> <repo>
sys.path.insert(0, str(REPO_ROOT / "sdk" / "python"))

try:
    import csudb  # type: ignore
except Exception as exc:  # pragma: no cover
    print("无法导入 sdk/python/csudb.py:", exc)
    sys.exit(2)

from cases import ALL_SCENARIOS  # noqa: E402

TEST_PASSWORD = "csudb_test_pwd"
CONNECT_TIMEOUT = 5.0

# --------------------------------------------------------------------------- #
# 结果分类
# --------------------------------------------------------------------------- #
PASS = "PASS"
FAIL = "FAIL"
CRASH = "CRASH"
WRONG_ACCEPT = "WRONG_ACCEPT"
WRONG_REJECT = "WRONG_REJECT"
WRONG_LOCATION = "WRONG_LOCATION"
WRONG_RESULT = "WRONG_RESULT"
SETUP_ERROR = "SETUP_ERROR"


def is_crash(message: str) -> bool:
    text = (message or "").lower()
    return any(
        k in text
        for k in (
            "closed the connection",
            "broken pipe",
            "network error",
            "connection refused",
            "connection reset",
            "server has gone away",
        )
    )


def parse_location(message: str):
    m = re.search(r"line\s+(-?\d+)\s*,\s*column\s+(-?\d+)", message or "")
    if not m:
        return None, None
    return int(m.group(1)), int(m.group(2))


# --------------------------------------------------------------------------- #
# 服务端管理
# --------------------------------------------------------------------------- #
def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def find_server_binary() -> Path:
    candidates = [
        REPO_ROOT / "build_debug" / "bin" / "csudbd",
        REPO_ROOT / "build" / "bin" / "csudbd",
        REPO_ROOT / "build_release" / "bin" / "csudbd",
    ]
    for c in candidates:
        if c.exists():
            return c
    found = shutil.which("csudbd") or shutil.which("observer")
    if found:
        return Path(found)
    raise SystemExit("未找到 csudbd 可执行文件，请先运行 ./build.sh debug --make")


class LocalServer:
    """使用临时数据目录启动 csudbd。"""

    def __init__(self, binary: Path):
        self.binary = binary
        self.datadir = None
        self.port = None
        self.proc = None
        self.log = None

    def start(self):
        self.datadir = tempfile.mkdtemp(prefix="csudb_sql_test_")
        self.port = free_port()
        env = dict(os.environ, CSUDB_INITIAL_ROOT_PASSWORD=TEST_PASSWORD)
        subprocess.run(
            [str(self.binary), "--initialize", "--data-dir", self.datadir],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        log_path = os.path.join(self.datadir, "server.log")
        self.log = open(log_path, "w")
        self.proc = subprocess.Popen(
            [str(self.binary), "--data-dir", self.datadir, "-p", str(self.port)],
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )
        deadline = time.time() + 30
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("csudbd 启动后立即退出，请查看 " + log_path)
            try:
                conn = csudb.connect("127.0.0.1", self.port, "root", TEST_PASSWORD, "", 2.0)
                conn.close()
                return
            except Exception:
                time.sleep(0.1)
        raise RuntimeError("csudbd 在 30s 内未就绪")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                self.proc.kill()
        if self.log:
            self.log.close()

    def cleanup(self):
        self.stop()
        if self.datadir and os.path.isdir(self.datadir):
            shutil.rmtree(self.datadir, ignore_errors=True)


# --------------------------------------------------------------------------- #
# 用例执行
# --------------------------------------------------------------------------- #
def execute(conn, sql):
    """返回 (kind, payload)：kind in {rows, ok, error, crash}"""
    try:
        cur = conn.execute(sql)
        if cur.description:
            return "rows", ([d[0] for d in cur.description], [list(r) for r in cur.fetchall()])
        return "ok", cur.rowcount
    except Exception as exc:  # noqa: BLE001
        msg = str(exc)
        if is_crash(msg):
            return "crash", msg
        return "error", msg


def rows_equal(got, want, ordered):
    got = [tuple(str(c) for c in row) for row in got]
    want = [tuple(str(c) for c in row) for row in want]
    if ordered:
        return got == want
    return sorted(got) == sorted(want)


def run_completion(conn, sql, cursor=None, want_model=False, max_items=20):
    if cursor is None:
        cursor = len(sql)
    response = conn._request(
        {"type": "complete", "sql": sql, "cursor": cursor, "max_items": max_items, "want_model": want_model}
    )
    if not response.get("success", False):
        raise RuntimeError(response.get("message") or "completion request failed")
    return [row[0] for row in response.get("rows", [])]


def evaluate_completion(conn, chk):
    spec = chk["complete"]
    try:
        items = run_completion(conn, spec["sql"], spec.get("cursor"), spec.get("want_model", False))
    except Exception as exc:  # noqa: BLE001
        if is_crash(str(exc)):
            return CRASH, str(exc)
        return FAIL, "补全请求失败: %s" % exc
    for sub in spec.get("contains", []) or []:
        if sub not in items:
            return FAIL, "缺少候选 %r，实际 %r" % (sub, items)
    for sub in spec.get("not_contains", []) or []:
        if sub in items:
            return FAIL, "不应出现候选 %r，实际 %r" % (sub, items)
    if spec.get("empty") and items:
        return FAIL, "预期无候选，实际 %r" % items
    return PASS, ""


def evaluate_check(conn, chk):
    """执行一条 check，返回 (status, detail)。"""
    if "complete" in chk:
        return evaluate_completion(conn, chk)

    sql = chk["sql"]
    kind, payload = execute(conn, sql)

    if kind == "crash":
        return CRASH, payload

    if "expect_error" in chk:
        spec = chk["expect_error"] or {}
        if kind != "error":
            return WRONG_ACCEPT, "预期报错，但执行成功: %r" % (payload,)
        msg = payload
        exp_type = spec.get("type")
        if exp_type and exp_type.lower() not in msg.lower():
            return FAIL, "错误类型不匹配: 期望 %s，实际 %s" % (exp_type, msg.splitlines()[0])
        for sub in spec.get("contains", []) or []:
            if sub.lower() not in msg.lower():
                return FAIL, "错误消息缺少 %r: %s" % (sub, msg)
        exp_line = spec.get("line")
        exp_col = spec.get("column")
        if exp_line is not None or exp_col is not None:
            line, col = parse_location(msg)
            if (exp_line is not None and line != exp_line) or (exp_col is not None and col != exp_col):
                return WRONG_LOCATION, "错误位置不匹配: 期望 (%s,%s)，实际 (%s,%s)" % (
                    exp_line,
                    exp_col,
                    line,
                    col,
                )
        return PASS, ""

    if kind == "error":
        return WRONG_REJECT, "预期执行成功，但报错: %s" % payload

    # 期望结果集
    if "expect_rows" in chk:
        if kind != "rows":
            return WRONG_REJECT, "预期返回结果集，但返回的是受影响行数(ok)"
        cols, got = payload
        exp_cols = chk.get("expect_cols")
        if exp_cols is not None and list(cols) != list(exp_cols):
            return FAIL, "列名不匹配: 期望 %s，实际 %s" % (exp_cols, cols)
        if not rows_equal(got, chk["expect_rows"], chk.get("ordered", False)):
            return WRONG_RESULT, "结果集不匹配: 期望 %s，实际 %s" % (chk["expect_rows"], got)
        return PASS, ""

    # 仅要求成功执行
    return PASS, ""


# --------------------------------------------------------------------------- #
# 主流程
# --------------------------------------------------------------------------- #
def connect(host, port, password, database):
    return csudb.connect(host, port, "root", password, database, CONNECT_TIMEOUT)


def run_all(args):
    scenarios = ALL_SCENARIOS
    if args.filter:
        key = args.filter.lower()
        scenarios = [s for s in scenarios if key in s["name"].lower() or key in s.get("category", "").lower()]

    local = None
    if args.host is None:
        local = LocalServer(find_server_binary())
        local.start()
        host, port, password = "127.0.0.1", local.port, TEST_PASSWORD
    else:
        host, port, password = args.host, args.port, args.password

    conn = connect(host, port, password, args.database)
    counts = {PASS: 0, FAIL: 0, CRASH: 0, WRONG_ACCEPT: 0, WRONG_REJECT: 0, WRONG_LOCATION: 0, WRONG_RESULT: 0, SETUP_ERROR: 0}
    failures = []
    aborted = False

    def restart_session():
        nonlocal conn
        if local is None:
            raise RuntimeError("连接到外部服务端时无法自动重启")
        try:
            conn.close()
        except Exception:
            pass
        local.cleanup()
        local.start()
        conn = connect(host, local.port, TEST_PASSWORD, args.database)

    try:
        for scenario in scenarios:
            name = scenario["name"]
            category = scenario.get("category", "")
            print("\n[%s] %s" % (category, name))

            setup_ok = True
            for stmt in scenario.get("setup", []):
                kind, payload = execute(conn, stmt)
                if kind == "crash":
                    counts[CRASH] += 1
                    failures.append((name, "<setup>", CRASH, payload))
                    print("  CRASH (setup): %s" % str(payload).splitlines()[0])
                    if local is not None:
                        restart_session()
                    setup_ok = False
                    break
                if kind == "error":
                    counts[SETUP_ERROR] += 1
                    failures.append((name, "<setup>", SETUP_ERROR, "setup 失败: %s -> %s" % (stmt, payload)))
                    print("  SETUP_ERROR: %s -> %s" % (stmt, str(payload).splitlines()[0]))
                    setup_ok = False
                    break

            if not setup_ok:
                if local is not None and aborted:
                    aborted = False
                continue

            for chk in scenario.get("checks", []):
                cname = chk.get("name", chk["sql"][:48])
                status, detail = evaluate_check(conn, chk)
                counts[status] = counts.get(status, 0) + 1
                if status == PASS:
                    if args.verbose:
                        print("  PASS  %s" % cname)
                else:
                    failures.append((name, cname, status, detail))
                    print("  %-14s %s" % (status, cname))
                    if detail:
                        print("        %s" % detail)
                if status == CRASH:
                    if local is not None:
                        print("  服务端崩溃，重启后继续...")
                        restart_session()
                    else:
                        aborted = True
                        break
            if aborted:
                print("外部服务端崩溃，终止后续用例。")
                break
    finally:
        try:
            conn.close()
        except Exception:
            pass
        if local is not None:
            local.cleanup()

    # ------------------------------------------------------------------ #
    total = sum(counts.values())
    print("\n" + "=" * 62)
    print("测试汇总: 共 %d 项" % total)
    for key in (PASS, FAIL, CRASH, WRONG_ACCEPT, WRONG_REJECT, WRONG_LOCATION, WRONG_RESULT, SETUP_ERROR):
        print("  %-14s %d" % (key, counts.get(key, 0)))
    if failures:
        print("\n失败明细:")
        for sc, cname, status, detail in failures:
            print("  [%s] %s | %s | %s" % (status, sc, cname, detail))
    print("=" * 62)

    ok = counts[FAIL] == 0 and counts[CRASH] == 0 and counts[WRONG_ACCEPT] == 0 and counts[WRONG_REJECT] == 0 \
        and counts[WRONG_LOCATION] == 0 and counts[WRONG_RESULT] == 0 and counts[SETUP_ERROR] == 0
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(description="CSUDB SQL 编译器与基础功能测试集")
    parser.add_argument("--host", default=None, help="已启动服务端地址；省略则自动拉起临时服务端")
    parser.add_argument("--port", type=int, default=6789, help="服务端端口")
    parser.add_argument("--password", default="", help="root 密码（连接外部服务端时使用）")
    parser.add_argument("--database", default="", help="使用的数据库")
    parser.add_argument("--filter", default="", help="只运行名称/分类包含该关键字的场景")
    parser.add_argument("--verbose", action="store_true", help="打印每个通过的用例")
    args = parser.parse_args()
    sys.exit(run_all(args))


if __name__ == "__main__":
    main()
