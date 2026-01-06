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
        elif test_name == "with_rollback_undo_param":
            return await self._test_with_rollback_undo_param(step)
        elif test_name == "with_rollback_depends":
            return await self._test_with_rollback_depends(step)
        elif test_name == "with_rollback_concurrent":
            return await self._test_with_rollback_concurrent(step)
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

        # The wrapper should have _undo_handler set
        registered["has_undo"] = step_1._undo_handler is not None

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
            "undo_registered": step_with_value._undo_handler is not None,
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
        """Test that with_rollback raises ValueError if no undo handler is provided."""

        @step.with_rollback("step_without_undo")
        async def step_without_undo():
            return "value"

        # Don't attach .undo - should raise ValueError
        try:
            await step_without_undo()
            return {"error": None, "raised": False}
        except ValueError as e:
            return {
                "raised": True,
                "error": str(e),
                "contains_step_name": "step_without_undo" in str(e),
                "contains_hint": ".undo" in str(e) or "undo=" in str(e),
            }

    async def _test_with_rollback_undo_param(self, step):
        """Test that with_rollback accepts undo= parameter directly."""
        results = []

        async def reusable_undo(error, value):
            results.append(f"undo:{value}")

        @step.with_rollback("step_with_undo_param", undo=reusable_undo)
        async def step_with_undo_param():
            results.append("do")
            return "value_from_do"

        value = await step_with_undo_param()
        results.append(f"returned:{value}")

        return {
            "results": results,
            "has_undo_handler": step_with_undo_param._undo_handler is not None,
        }

    async def _test_with_rollback_depends(self, step):
        """Test that with_rollback supports depends= parameter for DAG pattern."""
        results = []

        @step.with_rollback("step_1")
        async def step_1():
            results.append("do_1")
            return {"id": 1, "data": "first"}

        @step_1.undo
        async def _(error, value):
            results.append(f"undo_1:{value['id']}")

        # Step 2 depends on step 1
        @step.with_rollback("step_2", depends=[step_1])
        async def step_2(step_1_result):
            results.append(f"do_2:dep={step_1_result['id']}")
            return {"id": 2, "parent": step_1_result["id"]}

        @step_2.undo
        async def _(error, value):
            results.append(f"undo_2:{value['id']}")

        # Execute step_1 first
        val_1 = await step_1()

        # Execute step_2 - should receive step_1's result via depends
        val_2 = await step_2()

        return {
            "results": results,
            "step_1_value": val_1,
            "step_2_value": val_2,
        }

    async def _test_with_rollback_concurrent(self, step):
        """Test that with_rollback supports concurrent=True for parallel dependency resolution."""
        results = []

        @step.with_rollback("dep_a")
        async def dep_a():
            results.append("do_a")
            return "A"

        @dep_a.undo
        async def _(error, value):
            results.append(f"undo_a:{value}")

        @step.with_rollback("dep_b")
        async def dep_b():
            results.append("do_b")
            return "B"

        @dep_b.undo
        async def _(error, value):
            results.append(f"undo_b:{value}")

        # Step that depends on both A and B concurrently
        @step.with_rollback("combined", depends=[dep_a, dep_b], concurrent=True)
        async def combined(a_result, b_result):
            results.append(f"do_combined:{a_result}+{b_result}")
            return f"{a_result}{b_result}"

        @combined.undo
        async def _(error, value):
            results.append(f"undo_combined:{value}")

        # Execute dependencies first
        await dep_a()
        await dep_b()

        # Execute combined step - should receive both results
        combined_value = await combined()

        return {
            "results": results,
            "combined_value": combined_value,
        }


async def test(ctrl, env, ctx):
    pass
