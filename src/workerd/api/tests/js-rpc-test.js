import assert from 'node:assert';
import {StatelessService,DurableObject} from 'cloudflare:entrypoints';

export default {
  async noArgs(x, env, ctx) {
    return (x === undefined) ? (env.twelve + 1) : "param not undefined?";
  },

  async oneArg(i, env, ctx) {
    return env.twelve * i;
  },

  async twoArgs(i, env, ctx, j) {
    return i * j + env.twelve;
  },
}

export class MyService extends StatelessService {
  constructor(ctx, env) {
    super(ctx, env);
  }
}

export class MyActor extends DurableObject {
  constructor(state, env) {
    super(state, env);
  }
}

export let basicServiceBinding = {
  async test(controller, env, ctx) {
    // Test service binding RPC.
    assert.strictEqual(await env.self.noArgs(), 13);
    assert.strictEqual(await env.self.oneArg(3), 36);
    assert.strictEqual(await env.self.twoArgs(123, 2), 258);
  },
}

export let extendingEntrypointClasses = {
  async test(controller, env, ctx) {
    // Verify that we can instantiate classes that inherit built-in classes.
    let svc = new MyService(ctx, env);
    assert.equal(svc instanceof StatelessService, true);
  },
}
