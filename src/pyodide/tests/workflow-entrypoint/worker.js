// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
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
    const stubStep = new Context(true);

    // Tests forward compat - i.e.: python workflows should be compatible with steps that pass a ctx argument
    // this param is optional and searched by name. Meaning it's not positional
    const resp = await env.PythonWorkflow.run(
      {
        foo: 'bar',
      },
      stubStep
    );
    assert.deepStrictEqual(resp, 'foobar');
  },
};
