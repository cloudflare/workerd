import asyncio
import logging

import asgi
import js
from workers import Request, WorkerEntrypoint

from pyodide.ffi import to_js

# ---------------------------------------------------------------------------
# ASGI apps
# ---------------------------------------------------------------------------


def check_encoding(byte_str, encoding="utf-8"):
    try:
        byte_str.decode(encoding)
    except UnicodeDecodeError:
        return False
    return True


class HeaderEchoApp:
    """ASGI app that echoes request headers back in the response."""

    def __init__(self):
        pass

    async def __call__(self, scope, receive, send) -> None:
        scope["app"] = self

        assert scope["type"] in ("http", "websocket", "lifespan")

        if scope["type"] == "lifespan":
            message = await receive()
            if message["type"] == "lifespan.startup":
                await send({"type": "lifespan.startup.complete"})
            return

        elif scope["type"] == "http":
            headers = scope["headers"]
            for header in headers:
                assert isinstance(header[0], bytes) and isinstance(header[1], bytes)
                assert check_encoding(header[0]) and check_encoding(header[1])

            await receive()
            # Send response and return
            await send(
                {
                    "type": "http.response.start",
                    "status": 200,
                    "headers": headers,
                }
            )

            await send(
                {
                    "type": "http.response.body",
                    "body": b"Hello, World",
                }
            )


class SSEApp:
    """ASGI app that sends Server-Sent Events (multiple streamed chunks)."""

    async def __call__(self, scope, receive, send) -> None:
        scope["app"] = self

        assert scope["type"] in ("http", "websocket", "lifespan")

        if scope["type"] == "lifespan":
            message = await receive()
            if message["type"] == "lifespan.startup":
                await send({"type": "lifespan.startup.complete"})
            return

        elif scope["type"] == "http":
            await receive()

            # Send SSE response headers
            await send(
                {
                    "type": "http.response.start",
                    "status": 200,
                    "headers": [
                        (b"cache-control", b"no-store"),
                        (b"connection", b"keep-alive"),
                        (b"content-type", b"text/event-stream; charset=utf-8"),
                        (b"x-accel-buffering", b"no"),
                    ],
                }
            )

            # Send initial event
            await send(
                {
                    "type": "http.response.body",
                    "body": b"event: endpoint\r\ndata: /messages/?session_id=test123\r\n\r\n",
                    "more_body": True,
                }
            )

            # Send three ping events
            for i in range(3):
                # In a real app we would wait between events, but in the test we'll send them quickly
                await send(
                    {
                        "type": "http.response.body",
                        "body": f": ping - message {i + 1}\r\n\r\n".encode(),
                        "more_body": i < 2,  # last message has more_body=False
                    }
                )


STREAMING_CHUNK_SIZE = 1024
STREAMING_NUM_CHUNKS = 5


class StreamingApp:
    """ASGI app that streams multiple body chunks with a non-SSE content type.

    Reproduction test for bug https://github.com/cloudflare/workers-py/issues/67
    """

    async def __call__(self, scope, receive, send):
        if scope["type"] == "lifespan":
            message = await receive()
            if message["type"] == "lifespan.startup":
                await send({"type": "lifespan.startup.complete"})
            return

        if scope["type"] != "http":
            return

        # Consume the request body
        await receive()

        # Send response headers — NOT text/event-stream
        await send(
            {
                "type": "http.response.start",
                "status": 200,
                "headers": [
                    (b"content-type", b"application/octet-stream"),
                ],
            }
        )

        # Send multiple body chunks
        for i in range(STREAMING_NUM_CHUNKS):
            is_last = i == STREAMING_NUM_CHUNKS - 1
            # Each chunk is STREAMING_CHUNK_SIZE bytes filled with the chunk index
            chunk = bytes([i % 256]) * STREAMING_CHUNK_SIZE
            await send(
                {
                    "type": "http.response.body",
                    "body": chunk,
                    "more_body": not is_last,
                }
            )


# ---------------------------------------------------------------------------
# App instances and constants
# ---------------------------------------------------------------------------

app = HeaderEchoApp()
sse_app = SSEApp()
streaming_app = StreamingApp()

example_hdr = {"Header1": "Value1", "Header2": "Value2"}


class Default(WorkerEntrypoint):
    async def fetch(self, request):
        from js import URL

        url = URL.new(request.url)
        path = url.pathname

        if path == "/sse":
            return await asgi.fetch(sse_app, request, self.env, self.ctx)
        elif path == "/stream":
            return await asgi.fetch(streaming_app, request, self.env, self.ctx)

        # Verify that `asgi` can handle JS-style headers and Python-style headers:
        js_request = js.Request.new("http://example.com/", headers=to_js(example_hdr))
        py_request = Request("http://example.com/", headers=example_hdr)
        js_scope = asgi.request_to_scope(js_request, self.env)
        py_scope = asgi.request_to_scope(py_request, self.env)
        assert (
            js_scope["headers"]
            == py_scope["headers"]
            == [(k.lower().encode(), v.encode()) for k, v in example_hdr.items()]
        )

        # Standard asgi.fetch test path.
        return await asgi.fetch(app, request, self.env, self.ctx)

    async def test(self, ctrl):
        await test_headers(self.env)
        await test_sse(self.env)
        await test_streaming(self.env)
        await test_error_after_response_is_logged(self.env)
        await test_background_task_error_is_logged()
        await test_app_exception_before_response_is_logged()


# ---------------------------------------------------------------------------
# Test: headers
# ---------------------------------------------------------------------------


async def test_headers(env):
    response = await env.SELF.fetch("http://example.com/", headers=to_js(example_hdr))
    for header in response.headers.items():
        assert isinstance(header[0], str) and isinstance(header[1], str)
        expected_hdr = {k.lower(): v.lower() for k, v in example_hdr.items()}
        assert header[0] in expected_hdr.keys()
        assert expected_hdr[header[0]] == header[1].lower()


# ---------------------------------------------------------------------------
# Test: SSE (Server-Sent Events)
# ---------------------------------------------------------------------------


async def test_sse(env):
    response = await env.SELF.fetch("http://example.com/sse")

    assert response.headers["content-type"] == "text/event-stream; charset=utf-8"
    assert response.headers["cache-control"] == "no-store"

    from js import TextDecoder

    reader = response.body.getReader()
    content = ""
    decoder = TextDecoder.new()

    while True:
        result = await reader.read()
        if result.done:
            break
        chunk_text = decoder.decode(result.value, {"stream": True})
        content += chunk_text

    # Final flush
    content += decoder.decode()
    # Verify the expected events are in the response
    assert "event: endpoint" in content
    assert "data: /messages/?session_id=test123" in content
    assert ": ping - message 1" in content
    assert ": ping - message 2" in content
    assert ": ping - message 3" in content


# ---------------------------------------------------------------------------
# Test: non-SSE multi-chunk streaming
# Reproduction test for bug https://github.com/cloudflare/workers-py/issues/67
# ---------------------------------------------------------------------------


async def test_streaming(env):
    response = await env.SELF.fetch("http://example.com/stream")

    assert response.status == 200
    assert response.headers["content-type"] == "application/octet-stream"

    # Read the full response body via ReadableStream (same pattern as SSE test)
    reader = response.body.getReader()
    body_bytes = b""

    while True:
        result = await reader.read()
        if result.done:
            break
        body_bytes += result.value.to_bytes()

    expected_size = STREAMING_CHUNK_SIZE * STREAMING_NUM_CHUNKS  # 5120 bytes

    # This is the key assertion: with bug #10, only the first chunk (1024 bytes)
    # is returned. The fix should deliver all 5120 bytes.
    assert len(body_bytes) == expected_size, (
        f"Expected {expected_size} bytes, got {len(body_bytes)}. "
        f"Only {len(body_bytes) // STREAMING_CHUNK_SIZE} of {STREAMING_NUM_CHUNKS} chunks delivered."
    )

    # Verify each chunk has the correct content
    for i in range(STREAMING_NUM_CHUNKS):
        start = i * STREAMING_CHUNK_SIZE
        end = start + STREAMING_CHUNK_SIZE
        expected_byte = i % 256
        chunk = body_bytes[start:end]
        assert all(b == expected_byte for b in chunk), (
            f"Chunk {i}: expected all bytes to be {expected_byte}, "
            f"but got different content"
        )


# ---------------------------------------------------------------------------
# Logging tests
# ---------------------------------------------------------------------------


class _ListHandler(logging.Handler):
    """A logging handler that captures records into a list for assertions."""

    def __init__(self):
        super().__init__()
        self.records: list[logging.LogRecord] = []

    def emit(self, record):
        self.records.append(record)

    def clear(self):
        self.records.clear()


def _install_handler():
    """Install a ListHandler on the 'asgi' logger and return it."""
    handler = _ListHandler()
    logger = logging.getLogger("asgi")
    logger.addHandler(handler)
    # Ensure the logger level is low enough to capture everything.
    logger.setLevel(logging.DEBUG)
    return handler


def _remove_handler(handler):
    logging.getLogger("asgi").removeHandler(handler)


# -- Test: error after response already sent is logged (not swallowed) --------


class _ErrorAfterResponseApp:
    """ASGI app that sends a valid response, then raises an exception."""

    async def __call__(self, scope, receive, send):
        if scope["type"] == "lifespan":
            message = await receive()
            if message["type"] == "lifespan.startup":
                await send({"type": "lifespan.startup.complete"})
            return

        if scope["type"] == "http":
            await receive()
            await send(
                {
                    "type": "http.response.start",
                    "status": 200,
                    "headers": [(b"content-type", b"text/plain")],
                }
            )
            await send(
                {
                    "type": "http.response.body",
                    "body": b"ok",
                }
            )
            # Response is already sent — now raise an error.
            raise RuntimeError("post-response error for testing")


async def test_error_after_response_is_logged(env):
    handler = _install_handler()
    try:
        req = js.Request.new("http://example.com/log-test")
        # The response should still succeed — the error happens after it's sent.
        response = await asgi.fetch(_ErrorAfterResponseApp(), req, env)
        assert response.status == 200, f"Expected 200, got {response.status}"

        # consume the body
        await response.arrayBuffer()

        # Let the event loop run
        await asyncio.sleep(0.5)

        # The error should have been logged, not swallowed.
        error_records = [r for r in handler.records if r.levelno >= logging.ERROR]
        assert len(error_records) > 0, (
            "Expected at least one ERROR log record for post-response exception, "
            f"got {len(error_records)}. All records: {[r.getMessage() for r in handler.records]}"
        )
        matched = any(
            "post-response error for testing" in str(r.exc_info[1])
            if r.exc_info
            else False
            for r in error_records
        )
        assert matched, (
            "Expected log message containing 'post-response error for testing', "
            f"got: {[r.getMessage() for r in error_records]}"
        )
    finally:
        _remove_handler(handler)


# -- Test: background task error is logged ------------------------------------


async def test_background_task_error_is_logged():
    handler = _install_handler()
    try:

        async def failing_task():
            raise ValueError("background task failure for testing")

        asgi.run_in_background(failing_task())

        # Let the event loop run
        await asyncio.sleep(0.5)

        error_records = [r for r in handler.records if r.levelno >= logging.ERROR]
        assert len(error_records) > 0, (
            "Expected at least one ERROR log for background task failure, "
            f"got {len(error_records)}. All records: {[r.getMessage() for r in handler.records]}"
        )
        matched = any(
            "background task failure for testing" in str(r.exc_info[1])
            if r.exc_info
            else False
            for r in error_records
        )
        assert matched, (
            "Expected log message containing 'background task failure for testing', "
            f"got: {[r.getMessage() for r in error_records]}"
        )
    finally:
        _remove_handler(handler)


# -- Test: app exception before response is logged and re-raised -------------


class _ErrorBeforeResponseApp:
    """ASGI app that raises before sending any response."""

    async def __call__(self, scope, receive, send):
        if scope["type"] == "lifespan":
            message = await receive()
            if message["type"] == "lifespan.startup":
                await send({"type": "lifespan.startup.complete"})
            return

        if scope["type"] == "http":
            await receive()
            raise RuntimeError("app crash before response for testing")


async def test_app_exception_before_response_is_logged():
    handler = _install_handler()
    try:
        req = js.Request.new("http://example.com/crash-test")
        threw = False
        try:
            await asgi.fetch(_ErrorBeforeResponseApp(), req, {})
        except RuntimeError as e:
            threw = True
            assert "app crash before response for testing" in str(e), (
                f"Expected original exception message, got: {e}"
            )

        assert threw, "Expected RuntimeError to be raised from asgi.fetch"

        # fetch() should have logged the error before re-raising.
        error_records = [r for r in handler.records if r.levelno >= logging.ERROR]
        assert len(error_records) > 0, (
            "Expected at least one ERROR log for request failure, "
            f"got {len(error_records)}. All records: {[r.getMessage() for r in handler.records]}"
        )
        matched = any("ASGI request failed" in r.getMessage() for r in error_records)
        assert matched, (
            "Expected log message containing 'ASGI request failed', "
            f"got: {[r.getMessage() for r in error_records]}"
        )
    finally:
        _remove_handler(handler)
