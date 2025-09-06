import { RpcTarget, WorkflowEntrypoint } from 'cloudflare:workers';
import * as assert from 'node:assert';

class Context extends RpcTarget {
  async do(name, fn) {
    try {
      const result = await fn();
      return result;
    } catch (e) {
      console.log(`Error received: ${e.name} Message: ${e.message}`);
      // let's rethrow here since the engine does the same
      throw e;
    }
  }
}

export class WorkflowEntrypointTester extends WorkflowEntrypoint {
  __workflow_entrypoint = true;
  async run() {
    const step = new Context();
    return await step.do('foo', async () => {
      return 'bar';
    });
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
    assert.deepStrictEqual(resp, 'foobar');
  },
};
