#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Execute one process through a QEMU Guest Agent Unix socket."""

import argparse
import base64
import json
import socket
import sys
import time


class Protocol:
    """Line-oriented QGA protocol with response IDs and retained read-ahead."""

    def __init__(self, sock):
        self.sock = sock
        self.buffer = bytearray()
        self.sequence = 0

    def request(self, message):
        self.sequence += 1
        request_id = f"qga-exec-{self.sequence}"
        message = dict(message)
        message["id"] = request_id
        self.sock.sendall(
            json.dumps(message, separators=(",", ":")).encode() + b"\n"
        )

        while True:
            while b"\n" not in self.buffer:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise RuntimeError("guest agent closed the socket")
                self.buffer.extend(chunk)
            line, _, remainder = self.buffer.partition(b"\n")
            self.buffer = bytearray(remainder)
            if not line:
                continue
            response = json.loads(line)
            if response.get("id") != request_id:
                continue
            if "error" in response:
                raise RuntimeError(
                    json.dumps(response["error"], ensure_ascii=False)
                )
            return response.get("return")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("socket")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument(
        "--guest-encoding",
        default=None,
        help="decode captured output, for example gb18030 on Chinese Windows",
    )
    args = parser.parse_args()
    if not args.command:
        parser.error("a guest command is required")

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(args.timeout)
        sock.connect(args.socket)
        protocol = Protocol(sock)
        protocol.request({"execute": "guest-ping"})
        started = protocol.request(
            {
                "execute": "guest-exec",
                "arguments": {
                    "path": args.command[0],
                    "arg": args.command[1:],
                    "capture-output": True,
                },
            },
        )
        pid = started["pid"]
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            status = protocol.request(
                {"execute": "guest-exec-status", "arguments": {"pid": pid}},
            )
            if status.get("exited"):
                for key, target in (("out-data", sys.stdout), ("err-data", sys.stderr)):
                    if status.get(key):
                        payload = base64.b64decode(status[key])
                        if args.guest_encoding:
                            target.write(payload.decode(args.guest_encoding, "replace"))
                        else:
                            target.buffer.write(payload)
                return int(status.get("exitcode", 0))
            time.sleep(0.5)
    raise TimeoutError("guest command did not finish before timeout")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"qga-exec: {exc}", file=sys.stderr)
        raise SystemExit(1)
