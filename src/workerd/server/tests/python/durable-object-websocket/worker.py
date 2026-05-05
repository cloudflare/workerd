# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0
from js import WebSocketPair
from workers import DurableObject, Request, Response, WorkerEntrypoint


class DurableObjectWebSocket(DurableObject):
    def __init__(self, state, env):
        super().__init__(state, env)

    async def fetch(self, request):
        assert isinstance(request, Request)
        web_socket_pair = WebSocketPair.new()
        client, server = web_socket_pair
        self.ctx.acceptWebSocket(server)
        return Response(None, status=101, web_socket=client)

    async def webSocketMessage(self, ws, message):
        print("Received in Python DO WS message: ", message)
        ws.send("hello")

    async def webSocketClose(self, ws, code, reason, wasClean):
        print("Closed in Python DO WS")


class Default(WorkerEntrypoint):
    async def fetch(self, request):
        if request.method == "GET" and request.url.endswith("/websocket"):
            upgrade_header = request.headers.get("Upgrade")
            if upgrade_header != "websocket":
                return Response(
                    None,
                    status=426,
                    status_text="Expected Upgrade",
                    headers={"Content-Type": "text/plain"},
                )

            # We are explicitly testing usage via `self.env` here rather than the
            # argument to `fetch` and using `getByName` rather than `get`.
            stub = self.env.ns.getByName("A")
            return await stub.fetch(request)
        return Response("Not found", status=404)
