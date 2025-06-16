# Copyright (c) 2025 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from workers import WorkflowEntrypoint
from js import console

class WorkflowEntrypointExample(WorkflowEntrypoint):
    def __init__(self, state, env):
        self.state = state
        self.counter = 0

    async def on_run(self, event, step):
        @step.do("my_first_step")
        def workflow_step():
            self.counter += 1
            console.log(f"hello from python {self.counter}")

            return event["foo"]

        # Execute the decorated step and return its result so RPC receives it.
        return await workflow_step()

async def test(ctrl, env, ctx):
    pass
