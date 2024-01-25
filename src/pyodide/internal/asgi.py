from asyncio import Future, ensure_future, Queue
from inspect import isawaitable
from contextlib import contextmanager

ASGI = {"spec_version": "2.0", "version": "3.0"}


@contextmanager
def acquire_js_buffer(pybuffer):
    from pyodide.ffi import create_proxy

    px = create_proxy(pybuffer)
    buf = px.getBuffer()
    px.destroy()
    try:
        yield buf.data
    finally:
        buf.release()


def request_to_scope(req, ws=False):
    from js import URL

    headers = [tuple(x) for x in req.headers]
    url = URL.new(req.url)
    assert url.protocol[-1] == ':'
    scheme = url.protocol[:-1]
    path = url.pathname
    assert url.search[0] == '?'
    query_string = url.search[1:].encode()
    if ws:
        ty = "websocket"
    else:
        ty = "http"
    return {
        "asgi": ASGI,
        "headers": headers,
        "http_version": "1.1",
        "method": req.method,
        "scheme": scheme,
        "path": path,
        "query_string": query_string,
        "type": ty,
    }


async def start_application(app):
    it = iter([{"type": "lifespan.startup"}, Future()])

    async def receive():
        res = next(it)
        if isawaitable(res):
            await res
        return res

    ready = Future()

    async def send(got):
        print("Application startup complete.")
        print("Uvicorn running")
        ready.set_result(None)

    ensure_future(
        app(
            {
                "asgi": ASGI,
                "state": {},
                "type": "lifespan",
            },
            receive,
            send,
        )
    )
    await ready


async def process_request(app, req):
    from js import Response, Object
    from pyodide.ffi import create_proxy

    status = None
    headers = None
    result = Future()

    async def response_gen():
        async for data in req.body:
            yield {"body": data.to_bytes(), "more_body": True, "type": "http.request"}
        yield {"body": b"", "more_body": False, "type": "http.request"}

    responses = response_gen()

    async def receive():
        return await anext(responses)

    async def send(got):
        nonlocal status
        nonlocal headers
        if got["type"] == "http.response.start":
            status = got["status"]
            headers = got["headers"]
        if got["type"] == "http.response.body":
            # intentionally leak body to avoid a copy
            #
            # Apparently the `Response` constructor will not eagerly copy a
            # TypedArray argument, so if we don't leak the argument the response
            # body is corrupted. This is fine because the session is going to
            # exit at this point.
            px = create_proxy(got["body"])
            buf = px.getBuffer()
            px.destroy()
            resp = Response.new(
                buf.data, headers=Object.fromEntries(headers), status=status
            )
            result.set_result(resp)

    await app(request_to_scope(req), receive, send)
    return await result


async def process_websocket(app, req):
    from js import Response, WebSocketPair

    client, server = WebSocketPair.new().object_values()
    server.accept()
    queue = Queue()

    def onopen(evt):
        msg = {"type": "websocket.connect"}
        queue.put_nowait(msg)

    # onopen doesn't seem to get called. WS lifecycle events are a bit messed up
    # here.
    onopen(1)

    def onclose(evt):
        msg = {"type": "websocket.close", "code": evt.code, "reason": evt.reason}
        queue.put_nowait(msg)

    def onmessage(evt):
        msg = {"type": "websocket.receive", "text": evt.data}
        queue.put_nowait(msg)

    server.onopen = onopen
    server.onopen = onclose
    server.onmessage = onmessage

    async def ws_send(got):
        if got["type"] == "websocket.send":
            b = got.get("bytes", None)
            s = got.get("text", None)
            if b:
                with acquire_js_buffer(b) as jsbytes:
                    # Unlike the `Reponse` constructor,  server.send seems to
                    # eagerly copy the source buffer
                    server.send(jsbytes)
            if s:
                server.send(s)

        else:
            print(" == Not implemented", got["type"])

    async def ws_receive():
        received = await queue.get()
        return received

    ensure_future(app(request_to_scope(req, ws=True), ws_receive, ws_send))

    return Response.new(None, status=101, webSocket=client)


async def fetch(app, req):
    await start_application(app)
    return await process_request(app, req)


async def websocket(app, req):
    return await process_websocket(app, req)
