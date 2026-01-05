import { RpcTarget } from 'cloudflare:workers';
import * as assert from 'node:assert';

class Context extends RpcTarget {
  async do(name, configOrFn, maybeFn) {
    const fn = maybeFn ?? configOrFn;
    try {
      const result = await fn();
      return result;
    } catch (e) {
      console.log(`Error received: ${e.name} Message: ${e.message}`);
      throw e;
    }
  }
}

export default {
  async test(ctrl, env, ctx) {
    const stubStep = new Context();

    // Test 1: Basic rollback
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'basic_rollback' },
        stubStep
      );
      assert.deepStrictEqual(resp, [
        'do_1',
        'do_2',
        'undo_2:value_2',
        'undo_1:value_1',
      ]);
      console.log('✓ basic_rollback');
    }

    // Test 2: LIFO order
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'lifo_order' },
        stubStep
      );
      assert.deepStrictEqual(resp, ['third', 'second', 'first']);
      console.log('✓ lifo_order');
    }

    // Test 3: Steps without undo handlers
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'no_undo_handler' },
        stubStep
      );
      assert.deepStrictEqual(resp, [
        'do_with_undo',
        'do_without_undo',
        'undo_with_undo',
      ]);
      console.log('✓ no_undo_handler');
    }

    // Test 4: Undo receives value and error
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'undo_receives_value' },
        stubStep
      );
      assert.deepStrictEqual(resp.value, { id: 123, data: 'important' });
      assert.strictEqual(resp.error_msg, 'the error message');
      console.log('✓ undo_receives_value');
    }

    // Test 5: Undo with separate config
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'undo_with_config' },
        stubStep
      );
      assert.strictEqual(resp.undo, 1);
      console.log('✓ undo_with_config');
    }

    // Test 6: Empty undo stack is no-op
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'empty_undo_stack_noop' },
        stubStep
      );
      assert.strictEqual(resp.success, true);
      console.log('✓ empty_undo_stack_noop');
    }

    // Test 7: Nested rollback_all throws an error (engine's WorkflowFatalError)
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'nested_rollback_throws' },
        stubStep
      );
      // Engine throws WorkflowFatalError when rollbackAll is called during rollback
      assert.ok(resp.type !== null, 'Expected an error type');
      assert.ok(resp.message.includes('rollback'));
      console.log('✓ nested_rollback_throws');
    }

    // Test 8: Stop on first undo failure
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'stop_on_first_undo_failure' },
        stubStep
      );
      // LIFO order: undo_3 runs, undo_2 fails, undo_1 never runs
      assert.deepStrictEqual(resp, ['undo_3', 'undo_2']);
      console.log('✓ stop_on_first_undo_failure');
    }

    // Test 9: Rollback without error argument
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'rollback_without_error_arg' },
        stubStep
      );
      assert.deepStrictEqual(resp, ['undo_1:error=true']);
      console.log('✓ rollback_without_error_arg');
    }

    // Test 10: continue_on_error executes all undos
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'continue_on_error' },
        stubStep
      );
      // All undos should execute (LIFO: undo_3, undo_2 fails, undo_1)
      assert.deepStrictEqual(resp.executed, ['undo_3', 'undo_2', 'undo_1']);
      assert.strictEqual(resp.error.type, 'ExceptionGroup');
      assert.strictEqual(resp.error.count, 1);
      console.log('✓ continue_on_error');
    }

    console.log('All rollback tests passed!');
  },
};
