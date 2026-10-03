#!/usr/bin/env python3
import socket
import sys
import time
mode, host = sys.argv[1:]
if mode == 'server':
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind((host, 19090))
        s.settimeout(20)
        while True:
            data, peer = s.recvfrom(2048)
            s.sendto(data, peer)
            if data == b'quit':
                break
else:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(.2)
        for i in range(30):
            s.sendto(b'ready', (host, 19090))
            try:
                assert s.recvfrom(2048)[0] == b'ready'
                break
            except socket.timeout:
                time.sleep(.1)
        else:
            raise RuntimeError('UDP stack server did not reply')
        s.settimeout(5)
        for data in (b'', b'x', b'abc\x00def', bytes(bytearray(range(256))) * 5, b'z' * 1472):
            s.sendto(data, (host, 19090))
            reply, peer = s.recvfrom(2048)
            assert reply == data and peer == (host, 19090)
        s.sendto(b'quit', (host, 19090))
        assert s.recvfrom(2048)[0] == b'quit'
    print('PASS: kernel peer exchanged UDP binary/zero-length/1472-byte datagrams with stack server')
