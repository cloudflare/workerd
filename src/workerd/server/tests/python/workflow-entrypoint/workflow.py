# Copyright (c) 2025 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from workers import WorkflowEntrypoint


class WorkflowEntrypointExample(WorkflowEntrypoint):
    async def run(self, event, step):
        async def await_step(fn):
            try:
                return await fn()
            except TypeError as e:
                print(f"Successfully caught {type(e).__name__}: {e}")

        @step.do("my_failing")
        async def my_failing():
            print("Executing my_failing")
            raise TypeError("Intentional error in my_failing")

        @step.do("normal_step")
        async def normal_step():
            print("Executing normal step")
            return "done"

        await await_step(normal_step)

        @step.do("step_1")
        async def step_1():
            print("Executing step 1")
            return {"foo": "foo"}

        @step.do("step_2")
        async def step_2():
            print("Executing step 2")
            return {"bar": "bar"}

        # DAG example with error handling
        @step.do("step_3", depends=[my_failing, step_2], concurrent=True)
        async def step_3(_result1, _result2):
            # this should never run because one of the dependencies will fail
            pass

        await await_step(step_3)

        # `step_1` and `step_2` run serially
        @step.do("step_4", depends=[step_1, step_2], concurrent=False)
        async def step_4(result1, result2):
            print("Executing step 4 (depends on step 1 and step 2)")
            assert result1["foo"] == "foo"
            assert result2["bar"] == "bar"

        await await_step(step_4)

        @step.do("step_5", depends=[step_1, step_2], concurrent=False)
        async def step_5(result1, result2):
            print("Executing step 5 (depends on step 1 and step 2)")
            assert result1["foo"] == "foo"
            assert result2["bar"] == "bar"

            return result1["foo"] + result2["bar"]

        return await await_step(step_5)


async def test(ctrl, env, ctx):
    pass
