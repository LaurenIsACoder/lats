#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise both standalone QMP clients against silent and event-only peers."""
import ast
import json
from pathlib import Path
import socket
import tempfile
import threading
import time
from types import SimpleNamespace

KIT = Path(__file__).resolve().parent

def client(path):
    tree = ast.parse(path.read_text())
    node = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'QMP')
    created = []
    def create(*args, **kwargs):
        result=socket.socket(*args, **kwargs);created.append(result);return result
    proxy=SimpleNamespace(socket=create, AF_UNIX=socket.AF_UNIX, SOCK_STREAM=socket.SOCK_STREAM)
    namespace = dict(socket=proxy, json=json, time=time, created_sockets=created)
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(path), 'exec'), namespace)
    return namespace['QMP']

for source in ('run-tp-ras-compare.py', 'run-syscall-probe.py'):
    QMP = client(KIT/'scripts'/source)
    for behavior in ('normal', 'greeting-stall', 'request-stall', 'event-stream', 'eof', 'error'):
        with tempfile.TemporaryDirectory() as temp:
            address = str(Path(temp)/'qmp.sock')
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(address); listener.listen(1)
            stop = threading.Event()
            errors = []
            def peer():
                try:
                    connection, _ = listener.accept()
                    with connection:
                        if behavior == 'greeting-stall':
                            stop.wait(1); return
                        connection.sendall(b'{"QMP":{}}\n')
                        io = connection.makefile('rwb', buffering=0)
                        try:
                            assert json.loads(io.readline())['execute']=='qmp_capabilities'
                            io.write(b'{"return":{}}\n')
                            assert json.loads(io.readline())['execute']=='stop'
                            if behavior=='normal':io.write(b'{"event":"STOP"}\n{"return":{}}\n')
                            elif behavior=='error':io.write(b'{"error":{"desc":"failed"}}\n')
                            elif behavior=='event-stream':
                                while not stop.wait(.005):io.write(b'{"event":"NO_REPLY"}\n')
                            elif behavior=='request-stall':stop.wait(1)
                        finally:io.close()
                except (BrokenPipeError, ConnectionResetError):pass
                except BaseException as error:errors.append(error)
            thread = threading.Thread(target=peer);thread.start()
            qmp=None;started=time.monotonic()
            try:
                qmp=QMP(address, timeout=.08)
                qmp.command('stop')
                assert behavior=='normal',behavior
            except TimeoutError:
                assert behavior in ('greeting-stall','request-stall','event-stream'),behavior
            except RuntimeError:
                assert behavior in ('eof','error'),behavior
            finally:
                if qmp:qmp.close();assert qmp.sock.fileno()==-1
                stop.set();thread.join(timeout=2);listener.close()
            assert not thread.is_alive() and not errors,errors
            assert all(sock.fileno()==-1 for sock in QMP.__init__.__globals__['created_sockets'])
            assert time.monotonic()-started<1,behavior
    print('PASS:',source,'QMP normal/stall/events/EOF/error deadlines')
