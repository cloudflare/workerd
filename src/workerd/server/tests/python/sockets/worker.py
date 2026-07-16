import asyncio
import socket

from workers import WorkerEntrypoint


def recv_all(sock):
    chunks = []
    while True:
        chunk = sock.recv(4096)
        if chunk == b"":
            break
        chunks.append(chunk)
    return b"".join(chunks)


def recv_line(sock):
    data = bytearray()
    while not data.endswith(b"\n"):
        chunk = sock.recv(1)
        if chunk == b"":
            break
        data.extend(chunk)
    return bytes(data)


class Default(WorkerEntrypoint):
    def connect(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(
            (self.env.SIDECAR_HOSTNAME, int(self.env.PYTHON_SOCKET_SERVER_PORT))
        )
        return sock

    def test_basic_send_recv(self):
        message = b"BASIC hello from python\n"
        expected = b"echo: hello from python\n"

        sock = self.connect()
        try:
            sock.sendall(message)
            response = recv_line(sock)
        finally:
            sock.close()

        assert response == expected

    def test_multiple_send_recv(self):
        sock = self.connect()
        try:
            for message in [b"first", b"second", b"third"]:
                sock.sendall(b"ECHO " + message + b"\n")
                assert recv_line(sock) == message + b"\n"
            sock.sendall(b"BYE\n")
        finally:
            sock.close()

    def test_large_send(self):
        data = b"x" * (64 * 1024)
        sock = self.connect()
        try:
            sock.sendall(f"COUNT {len(data)}\n".encode())
            sock.sendall(data)
            assert recv_line(sock) == f"COUNTED {len(data)}\n".encode()
        finally:
            sock.close()

    def test_large_recv(self):
        size = 64 * 1024
        sock = self.connect()
        try:
            sock.sendall(f"SEND {size}\n".encode())
            data = recv_all(sock)
        finally:
            sock.close()

        assert len(data) == size
        assert data == b"y" * size

    def test_recv_backpressure(self):
        size = 1024 * 1024
        sock = self.connect()
        try:
            sock.sendall(f"SEND {size}\n".encode())
            data = recv_all(sock)
        finally:
            sock.close()

        assert len(data) == size
        assert data == b"y" * size

    def test_partial_recv(self):
        size = 1000
        sock = self.connect()
        try:
            sock.sendall(f"SEND {size}\n".encode())
            received = bytearray()
            while len(received) < size:
                chunk = sock.recv(100)
                if chunk == b"":
                    break
                assert len(chunk) <= 100
                received.extend(chunk)
        finally:
            sock.close()

        assert len(received) == size
        assert bytes(received) == b"y" * size

    def test_socket_metadata(self):
        sock = self.connect()
        try:
            fd_before = sock.fileno()
            peer = sock.getpeername()
            local = sock.getsockname()
            sock.sendall(b"BASIC metadata\n")
            assert recv_line(sock) == b"echo: metadata\n"
        finally:
            sock.close()

        assert isinstance(fd_before, int)
        assert fd_before > 0
        assert peer[0] == self.env.SIDECAR_HOSTNAME
        assert peer[1] == int(self.env.PYTHON_SOCKET_SERVER_PORT)
        assert isinstance(local[0], str)
        assert isinstance(local[1], int)

    def test_create_multiple(self):
        sock1 = self.connect()
        sock2 = self.connect()
        try:
            sock1.sendall(b"BASIC one\n")
            sock2.sendall(b"BASIC two\n")
            assert recv_line(sock1) == b"echo: one\n"
            assert recv_line(sock2) == b"echo: two\n"
        finally:
            sock1.close()
            sock2.close()

    def test_double_close_after_eof(self):
        sock = self.connect()
        sock.sendall(b"BASIC close twice\n")
        assert recv_line(sock) == b"echo: close twice\n"
        sock.close()
        sock.close()

    def test_recv_after_remote_close(self):
        sock = self.connect()
        try:
            sock.sendall(b"FINAL\n")
            assert recv_line(sock) == b"final message\n"
        finally:
            sock.close()

    def test_makefile(self):
        sock = self.connect()
        try:
            sock.sendall(b"LINES\n")
            f = sock.makefile("r")
            try:
                assert [line.strip() for line in f.readlines()] == [
                    "line1",
                    "line2",
                    "line3",
                ]
            finally:
                f.close()
        finally:
            sock.close()

    def test_connection_refused(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            try:
                sock.connect((self.env.SIDECAR_HOSTNAME, 1))
            except OSError:
                pass
            else:
                raise AssertionError("expected connection to closed port to fail")
        finally:
            sock.close()

    def test_settimeout_nonblocking_and_restore(self):
        sock = self.connect()
        try:
            sock.settimeout(0)
            try:
                sock.recv(1024)
            except OSError:
                pass
            else:
                raise AssertionError("expected nonblocking recv without data to fail")

            sock.settimeout(None)
            sock.sendall(b"BASIC asyncio\n")
            assert recv_line(sock) == b"echo: asyncio\n"
        finally:
            sock.close()

    def test_shutdown_after_eof(self):
        for how in [socket.SHUT_RD, socket.SHUT_WR, socket.SHUT_RDWR]:
            sock = self.connect()
            sock.sendall(b"BASIC shutdown\n")
            assert recv_line(sock) == b"echo: shutdown\n"
            sock.shutdown(how)
            sock.close()

    async def test_asyncio_sock_connect_recv_sendall(self):
        loop = asyncio.get_event_loop()
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setblocking(False)
        try:
            await loop.sock_connect(
                sock,
                (self.env.SIDECAR_HOSTNAME, int(self.env.PYTHON_SOCKET_SERVER_PORT)),
            )
            await loop.sock_sendall(sock, b"BASIC asyncio\n")
            assert await loop.sock_recv(sock, 1024) == b"echo: asyncio\n"
            assert await loop.sock_recv(sock, 1024) == b""
        finally:
            sock.close()

    async def test_asyncio_sock_recv_into(self):
        loop = asyncio.get_event_loop()
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setblocking(False)
        try:
            await loop.sock_connect(
                sock,
                (self.env.SIDECAR_HOSTNAME, int(self.env.PYTHON_SOCKET_SERVER_PORT)),
            )
            await loop.sock_sendall(sock, b"BASIC recv into\n")
            buffer = bytearray(1024)
            nbytes = await loop.sock_recv_into(sock, buffer)
            assert buffer[:nbytes] == b"echo: recv into\n"
            assert await loop.sock_recv(sock, 1024) == b""
        finally:
            sock.close()

    async def test_asyncio_open_connection(self):
        reader, writer = await asyncio.open_connection(
            self.env.SIDECAR_HOSTNAME,
            int(self.env.PYTHON_SOCKET_SERVER_PORT),
        )
        writer.write(b"BASIC stream\n")
        await writer.drain()
        assert await reader.readline() == b"echo: stream\n"
        assert await reader.read() == b""
        writer.close()

    async def test_asyncio_concurrent_connections(self):
        async def echo(message):
            reader, writer = await asyncio.open_connection(
                self.env.SIDECAR_HOSTNAME,
                int(self.env.PYTHON_SOCKET_SERVER_PORT),
            )
            writer.write(b"BASIC " + message + b"\n")
            await writer.drain()
            response = await reader.readline()
            assert await reader.read() == b""
            writer.close()
            return response

        assert await asyncio.gather(echo(b"one"), echo(b"two")) == [
            b"echo: one\n",
            b"echo: two\n",
        ]

    async def test(self):
        self.test_basic_send_recv()
        self.test_multiple_send_recv()
        self.test_large_send()
        self.test_large_recv()
        self.test_recv_backpressure()
        self.test_partial_recv()
        self.test_socket_metadata()
        self.test_create_multiple()
        self.test_double_close_after_eof()
        self.test_recv_after_remote_close()
        self.test_makefile()
        self.test_connection_refused()
        self.test_settimeout_nonblocking_and_restore()
        self.test_shutdown_after_eof()
        await self.test_asyncio_sock_connect_recv_sendall()
        await self.test_asyncio_sock_recv_into()
        await self.test_asyncio_open_connection()
        await self.test_asyncio_concurrent_connections()
