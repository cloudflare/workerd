# Copyright (c) 2025 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from workers import WorkflowEntrypoint


class WorkflowEntrypointExample(WorkflowEntrypoint):
    def __init__(self, state, env):
        self.state = state
        self.counter = 0

    async def on_run(self, event, step):
        @step.do("my_first_step")
        async def workflow_step():
            self.counter += 1

            return event["foo"]

        await workflow_step()

        async def step_with_callback():
            self.counter += 1

            return event["foo"]

        return step.do("my_second_step", step_with_callback)


async def test(ctrl, env, ctx):
    pass
