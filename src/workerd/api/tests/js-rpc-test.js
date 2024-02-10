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
  #env;

  constructor(ctx, env) {
    super(ctx, env);
    this.#env = env;

    // This shouldn't be callable!
    this.instanceMethod = () => {
      return "nope";
    }
  }

  async noArgsMethod(x) {
    return (x === undefined) ? (this.#env.twelve + 1) : "param not undefined?";
  }

  async oneArgMethod(i) {
    return this.#env.twelve * i;
  }

  async twoArgsMethod(i, j) {
    return i * j + this.#env.twelve;
  }

  async fetch(req, x) {
    assert.strictEqual(x, undefined);
    return new Response("method = " + req.method + ", url = " + req.url);
  }
}

export class MyActor extends DurableObject {
  #counter = 0;

  constructor(state, env) {
    super(state, env);
  }

  async increment(amount) {
    this.#counter += amount;
    return this.#counter;
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

export let namedServiceBinding = {
  async test(controller, env, ctx) {
    assert.strictEqual(await env.MyService.noArgsMethod(), 13);
    assert.strictEqual(await env.MyService.oneArgMethod(3), 36);
    assert.strictEqual(await env.MyService.twoArgsMethod(123, 2), 258);

    // `fetch()` is special, the params get converted into a Request.
    let resp = await env.MyService.fetch("http://foo", {method: "POST"});
    assert.strictEqual(await resp.text(), "method = POST, url = http://foo");

    await assert.rejects(() => env.MyService.instanceMethod(), {
      name: "TypeError",
      message: "The RPC receiver does not implement the method \"instanceMethod\"."
    });
  },
}

export let namedActorBinding = {
  async test(controller, env, ctx) {
    let id = env.MyActor.idFromName("foo");
    let stub = env.MyActor.get(id);

    assert.strictEqual(await stub.increment(5), 5);
    assert.strictEqual(await stub.increment(2), 7);
    assert.strictEqual(await stub.increment(8), 15);
  },
}
