import assert from 'node:assert';
import {WorkerEntrypoint,DurableObject} from 'cloudflare:workers';

export let nonClass = {
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

export class MyService extends WorkerEntrypoint {
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

export class ActorNoExtends {
  async fetch(req) {
    return new Response("from ActorNoExtends");
  }

  // This can't be called!
  async foo() {
    return 123;
  }
}

export default class DefaultService extends WorkerEntrypoint {
  async fetch(req) {
    return new Response("default service");
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
    assert.equal(svc instanceof WorkerEntrypoint, true);
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

    let getByName = name => {
      let func = env.MyService.getRpcMethodForTestOnly(name);
      return func.bind(env.MyService);
    };

    // Check getRpcMethodForTestOnly() actually works.
    assert.strictEqual(await getByName("twoArgsMethod")(2, 3), 18);

    // Check we cannot call reserved methods.
    await assert.rejects(() => getByName("constructor")(), {
      name: "TypeError",
      message: "'constructor' is a reserved method and cannot be called over RPC."
    });
    await assert.rejects(() => getByName("fetch")(), {
      name: "TypeError",
      message: "'fetch' is a reserved method and cannot be called over RPC."
    });

    // Check we cannot call methods of Object.
    await assert.rejects(() => getByName("toString")(), {
      name: "TypeError",
      message: "The RPC receiver does not implement the method \"toString\"."
    });
    await assert.rejects(() => getByName("hasOwnProperty")(), {
      name: "TypeError",
      message: "The RPC receiver does not implement the method \"hasOwnProperty\"."
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

// Test that if the actor class doesn't extend `DurableObject`, we don't allow RPC.
export let actorWithoutExtendsRejectsRpc = {
  async test(controller, env, ctx) {
    let id = env.ActorNoExtends.idFromName("foo");
    let stub = env.ActorNoExtends.get(id);

    // fetch() works.
    let resp = await stub.fetch("http://foo");
    assert.strictEqual(await resp.text(), "from ActorNoExtends");

    // RPC does not.
    await assert.rejects(() => stub.foo(), {
      name: "TypeError",
      message:
          "The receiving Durable Object does not support RPC, because its class was not declared " +
          "with `extends DurableObject`. In order to enable RPC, make sure your class " +
          "extends the special class `DurableObject`, which can be imported from the module " +
          "\"cloudflare:workers\"."
    });
  },
}

// Test calling the default export when it is a class.
export let defaultExportClass = {
  async test(controller, env, ctx) {
    let resp = await env.defaultExport.fetch("http://foo");
    assert.strictEqual(await resp.text(), "default service");
  },
}
