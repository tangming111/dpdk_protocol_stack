#!/usr/bin/env python3
"""RESP byte-stream integration without a redis-py dependency (Python 3.5+)."""
import socket
import sys
import time
from concurrent.futures import ThreadPoolExecutor

host, port = sys.argv[1], int(sys.argv[2])

def wire(*args):
    data = []
    for arg in args:
        data.append(arg if isinstance(arg, bytes) else str(arg).encode())
    return b'*%d\r\n' % len(data) + b''.join(b'$%d\r\n' % len(a) + a + b'\r\n' for a in data)

def read_reply(f):
    line = f.readline()
    if not line.endswith(b'\r\n'):
        raise RuntimeError('truncated RESP header: %r' % line)
    kind, value = line[:1], line[1:-2]
    if kind == b'-':
        raise RuntimeError(value.decode())
    if kind == b'+':
        return value
    if kind == b':':
        return int(value)
    if kind == b'$':
        n = int(value)
        if n == -1:
            return None
        data = f.read(n)
        if len(data) != n or f.read(2) != b'\r\n':
            raise RuntimeError('truncated RESP body')
        return data
    raise RuntimeError('unexpected RESP kind: %r' % kind)

with socket.create_connection((host, port), 5) as s:
    s.settimeout(10)
    with s.makefile('rb') as f:
        value = bytes(bytearray(range(256))) * 1024
        s.sendall(wire('SET', 'dpdknet:binary', value))
        assert read_reply(f) == b'OK'
        s.sendall(wire('GET', 'dpdknet:binary'))
        assert read_reply(f) == value
        s.sendall(wire('PING') * 100)
        assert all(read_reply(f) == b'PONG' for _ in range(100))
        s.sendall(wire('BGSAVE'))
        assert read_reply(f) == b'Background saving started'
        deadline = time.monotonic() + 10
        while True:
            s.sendall(wire('INFO', 'persistence'))
            info = read_reply(f)
            if b'rdb_bgsave_in_progress:0\r\n' in info:
                assert b'rdb_last_bgsave_status:ok\r\n' in info
                break
            assert time.monotonic() < deadline
            time.sleep(.05)
        s.sendall(wire('PING'))
        assert read_reply(f) == b'PONG'

def worker(index):
    with socket.create_connection((host, port), 5) as client:
        client.settimeout(5)
        with client.makefile('rb') as f:
            for i in range(20):
                key = 'dpdknet:parallel:%d' % index
                value = ('value:%d:%d' % (index, i)).encode()
                client.sendall(wire('SET', key, value) + wire('GET', key))
                assert read_reply(f) == b'OK'
                assert read_reply(f) == value
with ThreadPoolExecutor(max_workers=4) as pool:
    list(pool.map(worker, range(4)))
print('PASS: Redis 256 KiB binary SET/GET, 100 pipelined PINGs, BGSAVE, 4 concurrent clients')
