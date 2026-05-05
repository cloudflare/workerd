import asyncio
import logging

import asgi
import js
from workers import Request, WorkerEntrypoint

from pyodide.ffi import to_js


def check_encoding(byte_str, encoding="utf-8"):
    try:
        byte_str.decode(encoding)
    except UnicodeDecodeError:
        return False
    return True


class Server:
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


example_hdr = {"Header1": "Value1", "Header2": "Value2"}


class Default(WorkerEntrypoint):
    async def fetch(self, request):
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
        await header_test(self.env)
        await test_error_after_response_is_logged(self.env)
        await test_background_task_error_is_logged()
        await test_app_exception_before_response_is_logged()


app = Server()


async def header_test(env):
    response = await env.SELF.fetch("http://example.com/", headers=to_js(example_hdr))
    for header in response.headers.items():
        assert isinstance(header[0], str) and isinstance(header[1], str)
        expected_hdr = {k.lower(): v.lower() for k, v in example_hdr.items()}
        assert header[0] in expected_hdr.keys()
        assert expected_hdr[header[0]] == header[1].lower()


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

        # Let the event loop run
        await asyncio.sleep(20)

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


async def test_background_task_error_is_logged():
    handler = _install_handler()
    try:

        async def failing_task():
            raise ValueError("background task failure for testing")

        asgi.run_in_background(failing_task())

        # Let the event loop run
        await asyncio.sleep(20)

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
