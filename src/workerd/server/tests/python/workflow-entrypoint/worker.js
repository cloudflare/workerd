import { RpcTarget } from 'cloudflare:workers';
import * as assert from 'node:assert';

class Context extends RpcTarget {
  constructor(shouldSendCtx) {
    super();
    this.shouldSendCtx = shouldSendCtx;
  }

  async do(name, fn) {
    try {
      const ctx = { attempt: '1', metadata: 'expected_return_metadata' };
      const result = this.shouldSendCtx ? await fn(ctx) : await fn();
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
    let stubStep = new Context(true);

    // Tests forward compat - i.e.: python workflows should be compatible with steps that pass a ctx argument
    // this param is optional and searched by name. Meaning it's not positional
    let resp = await env.PythonWorkflow.run(
      {
        foo: 'bar',
      },
      stubStep
    );
    assert.deepStrictEqual(resp, 'foobar');

    // Tests backwards compat for dependency resolution - previously deps were not following a name based
    // approach. Instead, they were passed in the same order as they were declared in the depends param
    // This test also doesn't pass any ctx to steps
    stubStep = new Context(false);
    resp = await env.PythonWorkflowDepends.run(
      {
        foo: 'bar',
      },
      stubStep
    );
    assert.deepStrictEqual(resp, 'foobar');

    // Tests forwards compat for dependency resolution - pass ctx onto workflow that runs with the legacy
    // code path
    stubStep = new Context(true);
    resp = await env.PythonWorkflowDepends.run(
      {
        foo: 'bar',
      },
      stubStep
    );
    assert.deepStrictEqual(resp, 'foobar');
  },
};
