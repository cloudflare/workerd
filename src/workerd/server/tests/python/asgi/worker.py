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


async def on_fetch(request, env):
    import asgi

    return await asgi.fetch(app, request, env)


app = Server()


async def header_test(env):
    example_hdr = {"Header1": "Value1", "Header2": "Value2"}
    response = await env.SELF.fetch("http://example.com/", headers=to_js(example_hdr))
    for header in response.headers:
        assert isinstance(header[0], str) and isinstance(header[1], str)
        expected_hdr = {k.lower(): v.lower() for k, v in example_hdr.items()}
        assert header[0] in expected_hdr.keys()
        assert expected_hdr[header[0]] == header[1].lower()


async def test(ctrl, env):
    await header_test(env)
