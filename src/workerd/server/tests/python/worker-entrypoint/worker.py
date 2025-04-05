# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from workers import Response, WorkerEntrypoint


class WorkerEntrypointExample(WorkerEntrypoint):
    def __init__(self, state, env):
        self.state = state
        self.counter = 0

    async def rpc_trigger_func(self):
        self.counter += 1
        return Response(f"hello from python {self.counter}")


async def test(ctrl, env, ctx):
    first_resp = await env.entrypoint.rpc_trigger_func()
    assert first_resp == "hello from python 1"

    second_resp = await env.entrypoint.rpc_trigger_func()
    assert second_resp == "hello from python 2"
