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

    // Test 1: Basic with_rollback - executes do and returns value
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'with_rollback_basic' },
        stubStep
      );
      assert.deepStrictEqual(resp, ['do_1', 'returned:value_1']);
      console.log('✓ with_rollback_basic');
    }

    // Test 2: Undo decorator properly registers handler
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'with_rollback_undo_decorator' },
        stubStep
      );
      assert.strictEqual(resp.has_undo, true);
      console.log('✓ with_rollback_undo_decorator');
    }

    // Test 3: Handler structure captures return value
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'with_rollback_undo_receives_value' },
        stubStep
      );
      assert.deepStrictEqual(resp.do_result, { id: 123, data: 'important' });
      assert.strictEqual(resp.undo_registered, true);
      console.log('✓ with_rollback_undo_receives_value');
    }

    // Test 4: Config and undoConfig are stored
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'with_rollback_config' },
        stubStep
      );
      assert.deepStrictEqual(resp.step_config, { retries: { limit: 1 } });
      assert.deepStrictEqual(resp.undo_config, { retries: { limit: 3 } });
      console.log('✓ with_rollback_config');
    }

    // Test 5: with_rollback works without undo handler
    {
      const resp = await env.PythonWorkflow.run(
        { test: 'with_rollback_no_undo' },
        stubStep
      );
      assert.deepStrictEqual(resp, ['do_without_undo', 'returned:value']);
      console.log('✓ with_rollback_no_undo');
    }

    console.log('All with_rollback tests passed!');
  },
};
