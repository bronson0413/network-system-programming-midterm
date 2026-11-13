#!/usr/bin/env python3
"""啟動 server，送出各種異常封包與隨機竄改封包，檢查回應並確認沒有殘留子程序。

用法：tests/malformed_test.py [server 執行檔] [fuzz 次數]
"""
import os
import random
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

PORT = 19555
IDLE_SEC = 2
results = []


def frame(op, data=b"", magic=0x4E53, version=1, length=None, crc=None):
    size = len(data) if length is None else length
    head = struct.pack("!HBBI", magic, version, op, size)
    value = zlib.crc32(head + data) if crc is None else crc
    return head + struct.pack("!I", value) + data


def connect():
    return socket.create_connection(("127.0.0.1", PORT), timeout=5)


def read_reply(sock):
    data = b""
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
            if len(data) >= 12 and len(data) >= 12 + struct.unpack("!I", data[4:8])[0]:
                break
    except OSError as exc:
        return "exception", repr(exc)
    if len(data) < 12:
        return "short", data
    length = struct.unpack("!I", data[4:8])[0]
    body = data[12:12 + length]
    if zlib.crc32(data[:8] + body) != struct.unpack("!I", data[8:12])[0]:
        return "bad_crc", body
    return data[3], body


def check(name, passed, detail=""):
    results.append(passed)
    print("[%s] %s %s" % ("PASS" if passed else "FAIL", name, detail))


def expect_error(name, raw, message):
    with connect() as sock:
        sock.sendall(raw)
        op, body = read_reply(sock)
    check(name, op == 0xFF and body == message, "reply=%r" % (body,))


def fuzz(rounds):
    rng = random.Random(20261110)
    for _ in range(rounds):
        raw = bytearray(frame(rng.choice([1, 2, 3, 4, 5, 6, 7, 8, 15, 119]),
                              rng.randbytes(rng.randrange(0, 200))))
        for _ in range(rng.randrange(1, 5)):
            raw[rng.randrange(len(raw))] = rng.randrange(256)
        with connect() as sock:
            sock.sendall(raw[:rng.randrange(1, len(raw) + 1)])


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./server"
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 500
    with tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False) as log:
        path = log.name
    with open(path, "w") as log:
        server = subprocess.Popen([binary, "-p", str(PORT), "-t", str(IDLE_SEC), "-m", "64"],
                                  stderr=log)
    time.sleep(0.7)

    with connect() as sock:
        sock.sendall(frame(1, b"abc"))
        check("valid ping", read_reply(sock) == (0x81, b"abc"))
    with connect() as sock:
        sock.sendall(frame(1, b"z" * 8192))
        check("max payload 8192", read_reply(sock) == (0x81, b"z" * 8192))
    with connect() as sock:
        sock.sendall(frame(0x77))
        reply = read_reply(sock)
        sock.sendall(frame(1, b"after"))
        check("unknown opcode keeps connection",
              reply == (0xFF, b"unknown opcode") and read_reply(sock) == (0x81, b"after"))

    expect_error("bad magic", frame(1, b"x", magic=0x1234), b"bad magic")
    expect_error("bad version", frame(1, b"x", version=9), b"unsupported version")
    expect_error("length 4 GiB", frame(1, length=0xFFFFFFFF), b"bad length")
    expect_error("length 8193", frame(1, length=8193), b"bad length")
    expect_error("bad crc", frame(1, b"abc", crc=1), b"CRC mismatch")

    for name, raw in (("partial header", frame(1, b"abc")[:7]),
                      ("partial payload", frame(1, b"abcdef")[:15])):
        with connect() as sock:
            sock.sendall(raw)
            start = time.time()
            reply = read_reply(sock)
            took = time.time() - start
        check(name + " times out", reply == (0xFF, b"idle timeout") and took < IDLE_SEC + 1,
              "%.1fs" % took)

    start = time.time()
    closed = False
    with connect() as sock:
        try:
            for byte in frame(1, b"a" * 40):
                sock.send(bytes([byte]))
                time.sleep(0.2)
        except OSError:
            closed = True
    took = time.time() - start
    check("slow sender cut off", closed and took < IDLE_SEC + 2, "%.1fs" % took)

    with socket.socket() as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        sock.connect(("127.0.0.1", PORT))
        sock.settimeout(0.05)
        try:
            for _ in range(30000):
                sock.sendall(frame(8))
        except OSError:
            pass
        time.sleep(IDLE_SEC * 2 + 2)

    fuzz(rounds)
    print("fuzzed %d mutated frames" % rounds)
    time.sleep(IDLE_SEC + 1)

    workers = subprocess.run(["ps", "-o", "pid=,stat=", "--ppid", str(server.pid)],
                             capture_output=True, text=True).stdout.strip()
    check("no leftover workers", workers == "", repr(workers))
    check("server still alive", server.poll() is None)
    server.terminate()
    check("clean shutdown", server.wait(5) == 0)

    with open(path) as log:
        text = log.read()
    os.unlink(path)
    faults = re.findall(r"AddressSanitizer|LeakSanitizer|runtime error.*", text)
    check("no sanitizer reports", not faults, str(faults[:3]))
    print("result: %d/%d passed" % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
