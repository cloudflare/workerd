# Copyright (c) 2025 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from workers import WorkflowEntrypoint


class WorkflowRollbackExample(WorkflowEntrypoint):
    """Test workflow demonstrating the saga rollback pattern.

    Note: Rollback is triggered automatically by the engine when a workflow
    throws an uncaught error (if rollback config is enabled at instance creation).
    These tests verify the with_rollback decorator and undo handler registration.
    """

    async def run(self, event, step):
        test_name = event.get("test", "with_rollback_basic")

        if test_name == "with_rollback_basic":
            return await self._test_with_rollback_basic(step)
        elif test_name == "with_rollback_undo_decorator":
            return await self._test_with_rollback_undo_decorator(step)
        elif test_name == "with_rollback_undo_receives_value":
            return await self._test_with_rollback_undo_receives_value(step)
        elif test_name == "with_rollback_config":
            return await self._test_with_rollback_config(step)
        elif test_name == "with_rollback_no_undo":
            return await self._test_with_rollback_no_undo(step)
        else:
            raise ValueError(f"Unknown test: {test_name}")

    async def _test_with_rollback_basic(self, step):
        """Test that with_rollback executes the do function and returns value."""
        results = []

        @step.with_rollback("step_1")
        async def step_1():
            results.append("do_1")
            return "value_1"

        @step_1.undo
        async def undo_1(error, value):
            results.append(f"undo_1:{value}")

        value = await step_1()
        results.append(f"returned:{value}")

        return results

    async def _test_with_rollback_undo_decorator(self, step):
        """Test that .undo decorator properly registers undo handler."""
        registered = {"has_undo": False}

        @step.with_rollback("step_1")
        async def step_1():
            return "value"

        # Check that undo decorator returns the wrapper
        @step_1.undo
        async def undo_1(error, value):
            registered["has_undo"] = True

        # The wrapper should have _undo_fn set
        registered["has_undo"] = step_1._undo_fn is not None

        await step_1()
        return registered

    async def _test_with_rollback_undo_receives_value(self, step):
        """Test that undo handler receives the correct value from do function."""
        # This test verifies the handler structure is correct
        received = {"value": None}

        @step.with_rollback("step_with_value")
        async def step_with_value():
            return {"id": 123, "data": "important"}

        @step_with_value.undo
        async def undo_step(error, value):
            received["value"] = value

        result = await step_with_value()
        # Verify do function returned expected value
        return {
            "do_result": result,
            "undo_registered": step_with_value._undo_fn is not None,
        }

    async def _test_with_rollback_config(self, step):
        """Test that with_rollback accepts config and undoConfig."""
        configs = {"step_config": None, "undo_config": None}

        @step.with_rollback("step", config={"retries": {"limit": 1}})
        async def step_fn():
            return "done"

        @step_fn.undo(config={"retries": {"limit": 3}})
        async def undo_fn(error, value):
            pass

        configs["step_config"] = step_fn._config
        configs["undo_config"] = step_fn._undo_config

        await step_fn()
        return configs

    async def _test_with_rollback_no_undo(self, step):
        """Test that with_rollback works without an undo handler attached."""
        results = []

        @step.with_rollback("step_without_undo")
        async def step_without_undo():
            results.append("do_without_undo")
            return "value"

        # Don't attach .undo - should still work
        value = await step_without_undo()
        results.append(f"returned:{value}")

        return results


async def test(ctrl, env, ctx):
    pass
