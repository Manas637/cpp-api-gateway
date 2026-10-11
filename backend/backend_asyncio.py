#!/usr/bin/env python3
"""Asyncio-based sample backend.

Promoted from research variant with minimal hardening for controlled
benchmarking environment.
"""
import argparse
import asyncio
import json
import time


async def _handle(reader, writer, name, port, delay_ms):
    try:
        while True:
            request_line = await reader.readline()
            if not request_line:
                break
            # Skip empty lines
            if request_line in (b"\r\n", b"\n"):
                continue
            try:
                parts = request_line.rstrip()
                # split request line
                parts_split = parts.split()
                if len(parts_split) < 2:
                    break  # malformed
                method = parts_split[0].decode(errors='replace').upper()
                path = parts_split[1].decode(errors='replace')
                # ignore version if present
            except Exception:
                break  # malformed

            close = False
            headers_read = 0
            while True:
                header = await reader.readline()
                if not header:
                    return
                headers_read += 1
                if headers_read > 100:  # basic header limit
                    break
                if header in (b"\r\n", b"\n", b""):
                    break
                try:
                    if header.lower().startswith(b"connection:"):
                        if b"close" in header.lower():
                            close = True
                except Exception:
                    pass

            if delay_ms > 0:
                await asyncio.sleep(delay_ms / 1000.0)

            if path == "/health":
                obj = {"status": "ok", "backend": name}
            else:
                obj = {"backend": name, "port": port}

            body = json.dumps(obj).encode()
            head = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: application/json\r\n"
                b"Content-Length: " + str(len(body)).encode() + b"\r\n"
                b"Connection: " + (b"close" if close else b"keep-alive") + b"\r\n"
                b"\r\n"
            )
            writer.write(head + body)
            await writer.drain()

            if close:
                break
    except (ConnectionResetError, BrokenPipeError, asyncio.IncompleteReadError, TimeoutError):
        pass
    finally:
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass


async def run_asyncio(host, port, name, delay_ms, backlog):
    server = await asyncio.start_server(
        lambda r, w: _handle(r, w, name, port, delay_ms),
        host,
        port,
        backlog=backlog,
    )
    async with server:
        await server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description="Asyncio sample backend")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--host", type=str, default="0.0.0.0")
    parser.add_argument("--name", type=str, default=None)
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--backlog", type=int, default=5)
    parser.add_argument("--mode", choices=["asyncio"], default="asyncio")

    args = parser.parse_args()

    if args.delay_ms < 0:
        raise ValueError("delay-ms must be non-negative")

    name = args.name or f"server-{args.port}"
    asyncio.run(run_asyncio(args.host, args.port, name, args.delay_ms, args.backlog))


if __name__ == "__main__":
    main()
