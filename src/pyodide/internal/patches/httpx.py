"""A patch to make async httpx work using JavaScript fetch."""

from contextlib import contextmanager

from httpx._client import AsyncClient, BoundAsyncStream, logger
from httpx._models import Headers, Request, Response
from httpx._transports.default import AsyncResponseStream
from httpx._types import AsyncByteStream
from httpx._utils import Timer

from js import Headers as js_Headers
from js import fetch
from pyodide.ffi import create_proxy


@contextmanager
def acquire_buffer(content):
    """Acquire a Uint8Array view of a bytes object"""
    if not content:
        yield None
        return
    body_px = create_proxy(content)
    body_buf = body_px.getBuffer("u8")
    try:
        yield body_buf.data
    finally:
        body_px.destroy()
        body_buf.release()


async def js_readable_stream_iter(js_readable_stream):
    """Readable streams are supposed to be async iterators some day but they aren't yet.
    In the meantime, this is an adaptor that produces an async iterator from a readable stream.
    """
    reader = js_readable_stream.getReader()
    while True:
        res = await reader.read()
        if res.done:
            return
        b = res.value.to_bytes()
        print("js_readable_stream_iter", b)
        yield b


async def _send_single_request(self, request: Request) -> Response:
    """
    Sends a single request, without handling any redirections.

    This is the function we're patching here...
    """
    timer = Timer()
    await timer.async_start()

    if not isinstance(request.stream, AsyncByteStream):
        raise RuntimeError(
            "Attempted to send an sync request with an AsyncClient instance."
        )

    # BEGIN MODIFIED PART
    js_headers = js_Headers.new(request.headers.multi_items())
    with acquire_buffer(request.content) as body:
        js_resp = await fetch(
            str(request.url), method=request.method, headers=js_headers, body=body
        )

    py_headers = Headers(js_resp.headers)
    # Unset content-encoding b/c Javascript fetch already handled unpacking. If we leave it we will
    # get errors when httpx tries to unpack a second time.
    py_headers.pop("content-encoding", None)
    response = Response(
        status_code=js_resp.status,
        headers=py_headers,
        stream=AsyncResponseStream(js_readable_stream_iter(js_resp.body)),
    )
    # END MODIFIED PART

    assert isinstance(response.stream, AsyncByteStream)
    response.request = request
    response.stream = BoundAsyncStream(response.stream, response=response, timer=timer)
    self.cookies.extract_cookies(response)
    response.default_encoding = self._default_encoding

    logger.info(
        'HTTP Request: %s %s "%s %d %s"',
        request.method,
        request.url,
        response.http_version,
        response.status_code,
        response.reason_phrase,
    )

    return response


AsyncClient._send_single_request = _send_single_request
