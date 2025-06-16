import { RpcTarget } from "cloudflare:workers";
import * as assert from 'node:assert';

class Context extends RpcTarget {
  async do(name, fn) {
    return fn();
  }
}

export default {
  async test(ctrl, env, ctx) {
    // JS types
    const stubStep = new Context();

    const resp = await env.PythonWorkflow.run({
      foo: "bar",
    }, stubStep);
    assert.deepStrictEqual(resp, "bar");
  },
};
