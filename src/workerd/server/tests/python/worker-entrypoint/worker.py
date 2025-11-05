# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

import os

from workers import WorkerEntrypoint


class WorkerEntrypointExample(WorkerEntrypoint):
    def __init__(self, state, env):
        self.state = state
        self.counter = 0

    async def rpc_trigger_func(self):
        # Test that entropy works in WorkerEntrypoint...
        os.urandom(4)
        self.counter += 1
        return f"hello from python {self.counter}"


async def test(ctrl, env, ctx):
    first_resp = await env.entrypoint.rpc_trigger_func()
    assert first_resp == "hello from python 1"

    second_resp = await env.entrypoint.rpc_trigger_func()
    # We expect the counter to stay the same since this isn't a durable object.
    assert second_resp == "hello from python 1"
