#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Send key chords or ASCII text through a QMP Unix socket."""

import argparse
import json
import socket
import time


SHIFTED = {
    "_": "minus",
    ":": "semicolon",
    "!": "1",
    '"': "apostrophe",
    "+": "equal",
    "?": "slash",
}
PLAIN = {
    " ": "spc",
    "-": "minus",
    "=": "equal",
    ".": "dot",
    ",": "comma",
    ";": "semicolon",
    "'": "apostrophe",
    "/": "slash",
    "\\": "backslash",
}


class QMP:
    """Line-oriented QMP client which does not discard read-ahead events."""

    def __init__(self, path, timeout):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(path)
        self.buffer = bytearray()
        self.sequence = 0
        greeting = self.read_message()
        if "QMP" not in greeting:
            raise RuntimeError("missing QMP greeting")
        self.execute("qmp_capabilities")

    def read_message(self):
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("QMP closed the socket")
            self.buffer.extend(chunk)
        line, _, remainder = self.buffer.partition(b"\n")
        self.buffer = bytearray(remainder)
        return json.loads(line)

    def execute(self, command, arguments=None):
        self.sequence += 1
        request_id = f"qmp-type-{self.sequence}"
        message = {"execute": command, "id": request_id}
        if arguments is not None:
            message["arguments"] = arguments
        self.sock.sendall(
            json.dumps(message, separators=(",", ":")).encode() + b"\n"
        )
        while True:
            response = self.read_message()
            if response.get("id") != request_id:
                continue
            if "error" in response:
                raise RuntimeError(response["error"])
            return response.get("return")

    def keys(self, keys):
        for key in keys:
            self.key_event(key, True)
        for key in reversed(keys):
            self.key_event(key, False)

    def key_event(self, key, down):
        # Separate transitions prevent input backends from coalescing Ctrl+A
        # or Win+R into plain character input.
        self.execute(
            "input-send-event",
            {
                "events": [
                    {
                        "type": "key",
                        "data": {
                            "down": down,
                            "key": {"type": "qcode", "data": key},
                        },
                    }
                ]
            },
        )
        time.sleep(0.03)


def character_keys(character):
    if character.isalpha():
        key = character.lower()
        return ["shift", key] if character.isupper() else [key]
    if character.isdigit():
        return [character]
    if character in PLAIN:
        return [PLAIN[character]]
    if character in SHIFTED:
        return ["shift", SHIFTED[character]]
    raise ValueError(f"unsupported character: {character!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("socket")
    parser.add_argument("--chord", help="comma-separated QEMU qcodes")
    parser.add_argument("--text")
    parser.add_argument("--enter", action="store_true")
    parser.add_argument("--interval", type=float, default=0.025)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()
    if not args.chord and args.text is None and not args.enter:
        parser.error("specify --chord, --text, or --enter")

    qmp = QMP(args.socket, args.timeout)
    if args.chord:
        qmp.keys(args.chord.split(","))
        time.sleep(0.2)
    if args.text is not None:
        for character in args.text:
            qmp.keys(character_keys(character))
            time.sleep(args.interval)
    if args.enter:
        qmp.keys(["ret"])


if __name__ == "__main__":
    main()
