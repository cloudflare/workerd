# Copyright (c) 2025 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from workers import NonRetryableError, WorkflowEntrypoint


class MyCustomException(Exception):
    pass


class WorkflowEntrypointExample(WorkflowEntrypoint):
    async def on_run(self, event, step):
        @step.do("my_first_step")
        async def workflow_step():
            print("Doing workflow step... About to raise an error")
            raise TypeError("Test error")

        try:
            await workflow_step()
        except TypeError as e:
            print(f"successfully caught {e} with decorator")
            pass

        async def step_with_callback():
            print("Doing workflow step... About to raise an error")
            raise NonRetryableError("Test error")

        try:
            await step.do("my_second_step", step_with_callback)
        except NonRetryableError as e:
            print(f"successfully caught {e} with callback")
            pass

        @step.do("my_errored_step")
        async def workflow_step():
            print("Doing workflow step... About to raise an error")
            raise MyCustomException("Test error")

        try:
            await workflow_step()
        except MyCustomException as e:
            print(f"successfully caught {e} with decorator")
            pass

        @step.do("my_third_step")
        async def workflow_step_success():
            return event["foo"]

        try:
            return await workflow_step_success()
            # should not get thrown
        except Exception as e:
            print(f"Error in workflow step: {e}")


async def test(ctrl, env, ctx):
    pass
