import assert from 'node:assert';

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

export let basicServiceBinding = {
  async test(controller, env, ctx) {
    // Test service binding RPC.
    assert.strictEqual(await env.self.noArgs(), 13);
    assert.strictEqual(await env.self.oneArg(3), 36);
    assert.strictEqual(await env.self.twoArgs(123, 2), 258);
  },
}
