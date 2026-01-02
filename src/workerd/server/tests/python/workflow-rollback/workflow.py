# Copyright (c) 2025 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

from workers import WorkflowEntrypoint


class WorkflowRollbackExample(WorkflowEntrypoint):
    """Test workflow demonstrating the saga rollback pattern."""

    async def run(self, event, step):
        test_name = event.get("test", "basic_rollback")

        if test_name == "basic_rollback":
            return await self._test_basic_rollback(step)
        elif test_name == "lifo_order":
            return await self._test_lifo_order(step)
        elif test_name == "no_undo_handler":
            return await self._test_no_undo_handler(step)
        elif test_name == "undo_receives_value":
            return await self._test_undo_receives_value(step)
        elif test_name == "undo_with_config":
            return await self._test_undo_with_config(step)
        elif test_name == "empty_undo_stack_noop":
            return await self._test_empty_undo_stack_noop(step)
        elif test_name == "nested_rollback_throws":
            return await self._test_nested_rollback_throws(step)
        elif test_name == "stop_on_first_undo_failure":
            return await self._test_stop_on_first_undo_failure(step)
        elif test_name == "rollback_without_error_arg":
            return await self._test_rollback_without_error_arg(step)
        else:
            raise ValueError(f"Unknown test: {test_name}")

    async def _test_basic_rollback(self, step):
        """Test that rollback_all executes undo handlers."""
        results = []

        @step.with_rollback("step_1")
        async def step_1():
            results.append("do_1")
            return "value_1"

        @step_1.undo
        async def undo_1(error, value):
            results.append(f"undo_1:{value}")

        @step.with_rollback("step_2")
        async def step_2():
            results.append("do_2")
            return "value_2"

        @step_2.undo
        async def undo_2(error, value):
            results.append(f"undo_2:{value}")

        await step_1()
        await step_2()

        # Trigger rollback
        try:
            raise ValueError("test error")
        except ValueError as e:
            await step.rollback_all(e)

        return results

    async def _test_lifo_order(self, step):
        """Test that undos execute in LIFO (reverse) order."""
        order = []

        @step.with_rollback("first")
        async def first():
            return 1

        @first.undo
        async def undo_first(error, value):
            order.append("first")

        @step.with_rollback("second")
        async def second():
            return 2

        @second.undo
        async def undo_second(error, value):
            order.append("second")

        @step.with_rollback("third")
        async def third():
            return 3

        @third.undo
        async def undo_third(error, value):
            order.append("third")

        await first()
        await second()
        await third()

        await step.rollback_all(Exception("trigger"))

        # Should be LIFO: third, second, first
        return order

    async def _test_no_undo_handler(self, step):
        """Test that steps without undo handlers don't break rollback."""
        results = []

        @step.with_rollback("with_undo")
        async def with_undo():
            results.append("do_with_undo")
            return "value"

        @with_undo.undo
        async def undo_with(error, value):
            results.append("undo_with_undo")

        # Step without .undo decorator
        @step.with_rollback("without_undo")
        async def without_undo():
            results.append("do_without_undo")
            return "no_undo_value"

        await with_undo()
        await without_undo()

        await step.rollback_all(Exception("trigger"))

        # Only with_undo should have undo called
        return results

    async def _test_undo_receives_value(self, step):
        """Test that undo receives the original step's return value."""
        received = {}

        @step.with_rollback("step_with_value")
        async def step_with_value():
            return {"id": 123, "data": "important"}

        @step_with_value.undo
        async def undo_step(error, value):
            received["value"] = value
            received["error_msg"] = str(error)

        await step_with_value()
        await step.rollback_all(ValueError("the error message"))

        return received

    async def _test_undo_with_config(self, step):
        """Test that undo can have its own retry config."""
        call_count = {"undo": 0}

        @step.with_rollback("step", config={"retries": {"limit": 1}})
        async def step_fn():
            return "done"

        @step_fn.undo(config={"retries": {"limit": 3}})
        async def undo_fn(error, value):
            call_count["undo"] += 1
            return "undone"

        await step_fn()
        await step.rollback_all(Exception("trigger"))

        return call_count

    async def _test_empty_undo_stack_noop(self, step):
        """Test that rollback_all is a no-op when undo stack is empty."""

        # Don't register any rollback handlers
        @step.do("regular_step")
        async def regular_step():
            return "done"

        await regular_step()

        # This should not raise or do anything
        await step.rollback_all(Exception("trigger"))

        return {"success": True}

    async def _test_nested_rollback_throws(self, step):
        """Test that calling rollback_all during rollback throws RuntimeError."""
        error_caught = {"type": None, "message": None}

        @step.with_rollback("step_1")
        async def step_1():
            return "value"

        @step_1.undo
        async def undo_1(error, value):
            # Try to call rollback_all from within an undo handler
            try:
                await step.rollback_all(Exception("nested"))
            except RuntimeError as e:
                error_caught["type"] = "RuntimeError"
                error_caught["message"] = str(e)

        await step_1()
        await step.rollback_all(Exception("trigger"))

        return error_caught

    async def _test_stop_on_first_undo_failure(self, step):
        """Test that rollback stops on first undo failure by default."""
        executed = []

        @step.with_rollback("step_1")
        async def step_1():
            return 1

        @step_1.undo
        async def undo_1(error, value):
            executed.append("undo_1")

        @step.with_rollback("step_2")
        async def step_2():
            return 2

        @step_2.undo
        async def undo_2(error, value):
            executed.append("undo_2")
            raise Exception("undo_2 failed")

        @step.with_rollback("step_3")
        async def step_3():
            return 3

        @step_3.undo
        async def undo_3(error, value):
            executed.append("undo_3")

        await step_1()
        await step_2()
        await step_3()

        # Rollback should: undo_3 (ok), undo_2 (fail), stop before undo_1
        try:
            await step.rollback_all(Exception("trigger"))
        except Exception:
            pass  # Expected to fail

        return executed

    async def _test_rollback_without_error_arg(self, step):
        """Test that rollback_all works when called without an error argument."""
        results = []

        @step.with_rollback("step_1")
        async def step_1():
            return "value"

        @step_1.undo
        async def undo_1(error, value):
            results.append(f"undo_1:error={error is None}")

        await step_1()

        # Call without error - should use None
        await step.rollback_all(None)

        return results


async def test(ctrl, env, ctx):
    pass
