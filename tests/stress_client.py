#!/usr/bin/env python3
"""獨立的 Python 協定實作，測試並發、延遲與吞吐量（Linux 與 Windows 皆可執行）。

所有連線都建立完成後才同時開始送請求，確保量測期間連線確實重疊。
"""
import argparse
import asyncio
import statistics
import struct
import time
import zlib

MAGIC = 0x4E53
OP_PING = 0x01
OP_ALL = 0x08
OP_BYE = 0x0F
OP_BUSY = 0xFE
RESPONSE = 0x80


def frame(op, data=b""):
    head = struct.pack("!HBBI", MAGIC, 1, op, len(data))
    return head + struct.pack("!I", zlib.crc32(head + data)) + data


async def read_frame(reader):
    head = await reader.readexactly(12)
    magic, version, op, length, crc = struct.unpack("!HBBII", head)
    body = await reader.readexactly(length)
    if magic != MAGIC or version != 1 or zlib.crc32(head[:8] + body) != crc:
        raise ValueError("invalid frame")
    return op, body


async def request(reader, writer, op, data=b""):
    writer.write(frame(op, data))
    await writer.drain()
    return await read_frame(reader)


class Run:
    def __init__(self, total):
        self.total = total
        self.settled = 0
        self.all_settled = asyncio.Event()
        self.go = asyncio.Event()
        self.ok = 0
        self.busy = 0
        self.errors = []
        self.latencies = []

    def settle(self):
        self.settled += 1
        if self.settled == self.total:
            self.all_settled.set()


async def session(run, host, port, requests):
    writer = None
    settled = False
    try:
        reader, writer = await asyncio.open_connection(host, port)
        op, _ = await request(reader, writer, OP_PING, b"ready")
        settled = True
        run.settle()
        if op == OP_BUSY:
            run.busy += 1
            return
        if op != OP_PING | RESPONSE:
            raise ValueError("unexpected opcode 0x%02x" % op)

        await run.go.wait()
        for _ in range(requests):
            start = time.perf_counter()
            op, body = await request(reader, writer, OP_ALL)
            run.latencies.append(time.perf_counter() - start)
            if op != OP_ALL | RESPONSE or b"worker_pid=" not in body:
                raise ValueError("bad reply 0x%02x" % op)

        op, _ = await request(reader, writer, OP_BYE)
        if op != OP_BYE | RESPONSE:
            raise ValueError("bad bye reply 0x%02x" % op)
        run.ok += 1
    except (OSError, EOFError, ValueError, asyncio.IncompleteReadError) as exc:
        run.errors.append(type(exc).__name__ + ": " + str(exc))
        if not settled:
            run.settle()
    finally:
        if writer is not None:
            writer.close()


def percentile(values, ratio):
    return values[min(len(values) - 1, int(len(values) * ratio))] * 1000


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("-n", "--connections", type=int, default=100)
    parser.add_argument("-r", "--requests", type=int, default=20)
    args = parser.parse_args()

    run = Run(args.connections)
    connect_start = time.perf_counter()
    tasks = [asyncio.create_task(session(run, args.host, args.port, args.requests))
             for _ in range(args.connections)]
    await run.all_settled.wait()
    connect_sec = time.perf_counter() - connect_start

    start = time.perf_counter()
    run.go.set()
    await asyncio.gather(*tasks)
    elapsed = time.perf_counter() - start

    lat = sorted(run.latencies)
    print("connections=%d ok=%d busy=%d errors=%d connect_sec=%.2f"
          % (args.connections, run.ok, run.busy, len(run.errors), connect_sec))
    if lat:
        print("requests=%d elapsed_sec=%.3f throughput_req_per_sec=%.0f" %
              (len(lat), elapsed, len(lat) / elapsed))
        print("latency_ms mean=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f" %
              (statistics.mean(lat) * 1000, percentile(lat, 0.50),
               percentile(lat, 0.95), percentile(lat, 0.99), lat[-1] * 1000))
    for message in sorted(set(run.errors))[:5]:
        print("error:", message)
    return 1 if run.errors else 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
