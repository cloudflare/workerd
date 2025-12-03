import { RpcTarget } from 'cloudflare:workers';
import * as assert from 'node:assert';

class Context extends RpcTarget {
  async do(name, fn) {
    try {
      const ctx = { attempt: '1', metadata: 'expected_return_metadata' };
      const result = await fn(ctx);
      return result;
    } catch (e) {
      console.log(`Error received: ${e.name} Message: ${e.message}`);
      // let's rethrow here since the engine does the same
      throw e;
    }
  }
}

export default {
  async test(ctrl, env, ctx) {
    // JS types
    const stubStep = new Context();

    let resp = await env.PythonWorkflow.run(
      {
        foo: 'bar',
      },
      stubStep
    );
    assert.deepStrictEqual(resp, 'foobar');

    resp = await env.PythonWorkflowWithCtx.run(
      {
        foo: 'bar',
      },
      stubStep
    );
    assert.deepStrictEqual(resp, 'expected_return_metadata');
  },
};
