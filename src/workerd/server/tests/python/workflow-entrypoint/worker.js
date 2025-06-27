import { RpcTarget } from 'cloudflare:workers';
import * as assert from 'node:assert';

class Context extends RpcTarget {
  async do(name, fn) {
    try {
      return await fn();
    } catch (e) {
      console.log(`Error received: ${e.name} Message: ${e.message}`);
      if (
        (e instanceof Error &&
          (e.name === 'NonRetryableError' ||
            e.message.startsWith('NonRetryableError'))) ||
        (e.message.startsWith('PythonError') &&
          e.message.includes('_workers.NonRetryableError'))
      ) {
        // we need this statement on the engine
      }
      // let's rethrow here since the engine does the same
      throw e;
    }
  }
}

export default {
  async test(ctrl, env, ctx) {
    // JS types
    const stubStep = new Context();

    const resp = await env.PythonWorkflow.run(
      {
        foo: 'bar',
      },
      stubStep
    );
    assert.deepStrictEqual(resp, 'bar');
  },
};
