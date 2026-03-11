class SSEServer:
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
            # Receive the request
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


from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    async def fetch(self, request):
        import asgi

        return await asgi.fetch(app, request, self.env, self.ctx)

    async def test(self, ctrl):
        await test_sse(self.env)


app = SSEServer()


async def test_sse(env):
    # Make a request to our SSE endpoint
    response = await env.SELF.fetch("http://example.com/sse")

    # Verify the response has the correct headers for SSE
    assert response.headers["content-type"] == "text/event-stream; charset=utf-8"
    assert response.headers["cache-control"] == "no-store"

    # Use a simple method to convert the stream to text
    from js import TextDecoder

    # Read the stream in a simpler way
    reader = response.body.getReader()
    content = ""
    decoder = TextDecoder.new()

    while True:
        result = await reader.read()
        if result.done:
            break
        # Use TextDecoder to convert the chunk to text
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
