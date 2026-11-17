# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

import os
import re
import signal
import sys

from workers import WorkerEntrypoint

# The runtime imports its introspection module once, after this top level has run. A dedicated
# snapshot is collected after that, so it holds this hook and its count, and an isolate restored
# from it finds the module in sys.modules instead of compiling it again.
introspection_compiles = 0


def count_introspection_compiles(event, args):
    global introspection_compiles
    if event == "compile" and re.search(
        r"/_cloudflare/introspection_\w+\.py$", str(args[1])
    ):
        introspection_compiles += 1


sys.addaudithook(count_introspection_compiles)


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

    assert introspection_compiles == 1, introspection_compiles

    ((introspection_name, introspection),) = (
        (name, mod)
        for name, mod in sys.modules.items()
        if name.startswith("_cloudflare.introspection_")
    )
    assert signal.getsignal(signal.SIGXCPU) is introspection.raise_cpu_limit_exceeded
