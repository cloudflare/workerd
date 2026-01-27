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
        async def normal_step(ctx):
            assert ctx["attempt"] == "1"
            return "done"

        await normal_step()

        @step.do()
        async def step_1():
            print("Executing step 1")
            return {"foo": "foo"}

        @step.do()
        async def step_2():
            print("Executing step 2")
            return {"bar": "bar"}

        # DAG example with error handling
        @step.do("step_3", concurrent=True)
        async def step_3():
            # this should never run because one of the dependencies will fail
            pass

        await await_step(step_3)

        # `step_1` and `step_2` run serially
        @step.do("step_4", concurrent=True)
        async def step_4(step_1, ctx, step_2):
            assert ctx["attempt"] == "1"
            print("Executing step 4 (depends on step 1 and step 2)")
            assert step_1["foo"] == "foo"
            assert step_2["bar"] == "bar"

        await await_step(step_4)

        # tests step memoization - steps 1 and 2 are already resolved
        @step.do("step_5", concurrent=False)
        async def step_5(ctx, step_1=(), step_2=()):
            print("Executing step 5 (depends on step 1 and step 2)")
            assert step_1["foo"] == "foo"
            assert step_2["bar"] == "bar"

            return step_1["foo"] + step_2["bar"]

        return await step_5()


async def test(ctrl, env, ctx):
    pass
