# ------------------------------------------------------------------------------------------------
# 本文件功能索引（VSCode / Cursor：Ctrl+Click `文件:行号` 跳转）
# ------------------------------------------------------------------------------------------------
#  tcp_proxy.py:36            pipe
#  tcp_proxy.py:59            handle
#  tcp_proxy.py:88            main
# ------------------------------------------------------------------------------------------------
"""CSUDB 公网协议嗅探代理（TCP Proxy）。

本模块在 0.0.0.0:8157 上监听来自公网的 TCP 连接，并在连接建立后通过
socket.recv(MSG_PEEK) 窥探（peek）客户端发来的前 8 个字节，而不真正消费数据。
利用 HTTP 请求行/方法具有固定前缀这一特点进行协议嗅探：

  - 若首字节匹配 HTTP 方法前缀（GET/POST/PUT/HEAD/OPTIONS/DELETE/CONNECT/PATCH），
    则判定为网页网关流量，转发到 127.0.0.1:8765（HTTP 网关）。
  - 否则判定为 CSUDB 原生协议（原生 JSON over TCP），转发到 127.0.0.1:6789。

判定完成后，代理与上游建立连接，并启动两个线程在两个方向上做双向数据转发
（bidirectional pipe），同时保持首字节仍在客户端缓冲区中（因为只窥探未消费），
从而对客户端和上游都完全透明。

设计要点：公网只暴露 8157 一个端口，HTTP 网关与数据库原生协议都只监听回环地址，
由本代理在入口处按协议分流。
"""

import socket, threading

# 代理对外监听地址（公网入口）。
LISTEN = ("0.0.0.0", 8157)
WEB = ("127.0.0.1", 8765)     # 网页网关（HTTP）
DB  = ("127.0.0.1", 6789)     # 数据库原生协议（原生 JSON over TCP）
# 用于协议嗅探的 HTTP 方法前缀：任一前缀命中即视为 HTTP 流量。
HTTP_PREFIXES = (b"GET ", b"POST", b"PUT ", b"HEAD", b"OPTI", b"DELE", b"CONN", b"PATC")


def pipe(a, b):
    """单向数据泵：把 a 收到的数据原样写入 b。

    在一个方向的连接上循环 recv/sendall，直到对端关闭（recv 返回空字节）。
    任意异常都被静默吞掉，最后统一关闭两端套接字的读写方向，确保另一方向的
    转发线程也能随之结束，不会造成连接泄漏。
    """
    try:
        while True:
            data = a.recv(65536)
            if not data:
                break
            b.sendall(data)
    except Exception:
        pass
    finally:
        for s in (a, b):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass


def handle(client):
    """处理单个客户端连接：嗅探协议并转发到对应后端。

    步骤：
      1. 用 recv(MSG_PEEK) 窥探首批字节，仅"看"不"取"，因此数据仍留在缓冲区中，
         后续双向转发可原样送达后端，协议嗅探对通信双方透明。
      2. 根据窥探结果与 HTTP 前缀表匹配，决定目标为 WEB 还是 DB。
      3. 与目标后端建立连接；任一环节失败则关闭客户端连接。
      4. 启动两个方向的 pipe 线程，实现全双工转发。
    """
    try:
        client.settimeout(10)
        peek = client.recv(8, socket.MSG_PEEK)   # 只窥探，不消费
    except Exception:
        client.close()
        return
    target = WEB if any(peek.startswith(p) for p in HTTP_PREFIXES) else DB
    try:
        upstream = socket.create_connection(target, timeout=10)
    except Exception:
        client.close()
        return
    client.settimeout(None)
    upstream.settimeout(None)
    # 双向转发：client->upstream 与 upstream->client 各一个守护线程。
    threading.Thread(target=pipe, args=(client, upstream), daemon=True).start()
    threading.Thread(target=pipe, args=(upstream, client), daemon=True).start()


def main():
    """程序入口：创建监听套接字并循环接受连接，每个连接交给一个线程处理。"""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # 置 SO_REUSEADDR，便于重启后立即复用 8157 端口。
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(LISTEN)
    srv.listen(128)
    print("0.0.0.0:8157 -> HTTP/8765 (web) | native/6789 (db)", flush=True)
    while True:
        client, _ = srv.accept()
        threading.Thread(target=handle, args=(client,), daemon=True).start()


if __name__ == "__main__":
    main()
