"""HTTP/3-only integration probe; no HTTP/1 or HTTP/2 fallback."""
import asyncio
import hashlib
import ssl
import sys
from aioquic.asyncio import connect, QuicConnectionProtocol
from aioquic.h3.connection import H3Connection
from aioquic.h3.events import HeadersReceived, DataReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import HandshakeCompleted, ConnectionTerminated

class Client(QuicConnectionProtocol):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.http = H3Connection(self._quic)
        self.pending = {}
        self.alpn = None

    def quic_event_received(self, event):
        if isinstance(event, HandshakeCompleted):
            self.alpn = event.alpn_protocol
            print('QUIC handshake ALPN=' + str(self.alpn), flush=True)
        if isinstance(event, ConnectionTerminated):
            for future, _, _ in self.pending.values():
                if not future.done():
                    future.set_exception(RuntimeError(str(event)))
        for item in self.http.handle_event(event):
            if not isinstance(item, (HeadersReceived, DataReceived)):
                continue
            if item.stream_id not in self.pending:
                continue
            future, headers, body = self.pending[item.stream_id]
            if isinstance(item, HeadersReceived):
                headers.extend(item.headers)
            else:
                body.extend(item.data)
            if item.stream_ended and not future.done():
                future.set_result((dict(headers), bytes(body)))

    async def get(self, host, port, path, expected):
        stream = self._quic.get_next_available_stream_id()
        future = asyncio.get_running_loop().create_future()
        self.pending[stream] = (future, [], bytearray())
        self.http.send_headers(stream, [(b':method', b'GET'), (b':scheme', b'https'),
            (b':authority', ('%s:%s' % (host, port)).encode()),
            (b':path', path.encode())], end_stream=True)
        self.transmit()
        headers, body = await asyncio.wait_for(future, 20)
        del self.pending[stream]
        assert headers.get(b':status') == b'200', headers
        assert body == expected, (path, len(body), len(expected))
        assert self.alpn == 'h3', self.alpn
        print('PASS HTTP/3 stream=%d path=%s bytes=%d sha256=%s' %
              (stream, path, len(body), hashlib.sha256(body).hexdigest()), flush=True)

async def main():
    host, port = sys.argv[1], int(sys.argv[2])
    config = QuicConfiguration(is_client=True, alpn_protocols=['h3'])
    # Lab-only self-signed certificate, never use this setting for production.
    config.verify_mode = ssl.CERT_NONE
    config.idle_timeout = 15
    async with connect(host, port, configuration=config, create_protocol=Client) as client:
        await client.get(host, port, '/hello.txt', b'dpdk-nginx-http3\n')
        payload = bytes(range(256)) * 1024
        await client.get(host, port, '/payload.bin', payload)
        await asyncio.gather(*(client.get(host, port, '/payload.bin', payload) for _ in range(4)))
    print('PASS HTTP/3-only: 6 requests, including 4 concurrent streams', flush=True)

asyncio.run(asyncio.wait_for(main(), 90))
