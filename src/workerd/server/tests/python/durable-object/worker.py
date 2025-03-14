# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from workers import DurableObject, Response


class DurableObjectExample(DurableObject):
    def __init__(self, state, env):
        self.state = state
        self.counter = 0

    async def on_fetch(self, req, env, ctx):
        self.counter += 1
        return Response(f"hello from python {self.counter}")

    async def no_args_method(self):
        return "value from python"


async def test(ctrl, env, ctx):
    id = env.ns.idFromName("A")
    obj = env.ns.get(id)

    first_resp = await obj.fetch("http://foo/")
    first_resp_data = await first_resp.text()
    assert first_resp_data == "hello from python 1"

    second_resp = await obj.fetch("http://foo/")
    second_resp_data = await second_resp.text()
    assert second_resp_data == "hello from python 2"

    # TODO: Python effectively calls this via `obj.no_args_method.apply(obj, [])` which causes
    # a DataCloneError to occur. This may be a JS RPC bug that we will need to fix.
    # custom_resp = await obj.no_args_method()
    # assert custom_resp == "value from python: 42"
