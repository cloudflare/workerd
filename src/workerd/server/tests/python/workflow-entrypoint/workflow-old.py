# Copyright (c) 2025 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from workers import WorkflowEntrypoint


class PythonWorkflowDepends(WorkflowEntrypoint):
    # The purpose of this workflow is testing that depends are no longer resolved
    #
    async def run(self, event, step):
        @step.do("step_1")
        async def step_1():
            # tests backwards compat with workflows that don't have ctx in the step callback
            print("Executing step 1")
            return "foo"

        @step.do("step_2")
        async def step_2():
            print("Executing step 2")
            return "bar"

        @step.do("step_3", depends=[step_1, step_2], concurrent=True)
        async def step_3(result1=(), result2=()):
            assert result1 == "foo"
            assert result2 == "bar"
            return result1 + result2

        return await step_3()


async def test(ctrl, env, ctx):
    pass
