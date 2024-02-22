import assert from 'node:assert';
import {WorkerEntrypoint,DurableObject,RpcStub,RpcTarget} from 'cloudflare:workers';

class MyCounter extends RpcTarget {
  constructor(i = 0) {
    super();
    this.i = i;
  }

  async increment(j) {
    this.i += j;
    return this.i;
  }
}

export let nonClass = {
  async noArgs(x, env, ctx) {
    assert.strictEqual(typeof ctx.waitUntil, "function");
    return (x === undefined) ? (env.twelve + 1) : "param not undefined?";
  },

  async oneArg(i, env, ctx) {
    assert.strictEqual(typeof ctx.waitUntil, "function");
    return env.twelve * i;
  },

  async oneArgOmitCtx(i, env) {
    return env.twelve * i + 1;
  },

  async oneArgOmitEnvCtx(i) {
    return 2 * i;
  },

  async twoArgs(i, j, env, ctx) {
    assert.strictEqual(typeof ctx.waitUntil, "function");
    return i * j + env.twelve;
  },
}

export class MyService extends WorkerEntrypoint {
  constructor(ctx, env) {
    super(ctx, env);

    assert.strictEqual(this.ctx, ctx);
    assert.strictEqual(this.env, env);

    // This shouldn't be callable!
    this.instanceMethod = () => {
      return "nope";
    }
  }

  async noArgsMethod(x) {
    return (x === undefined) ? (this.env.twelve + 1) : "param not undefined?";
  }

  async oneArgMethod(i) {
    return this.env.twelve * i;
  }

  async twoArgsMethod(i, j) {
    return i * j + this.env.twelve;
  }

  async makeCounter(i) {
    return new MyCounter(i);
  }

  async incrementCounter(counter, i) {
    return await counter.increment(i);
  }

  async fetch(req, x) {
    assert.strictEqual(x, undefined);
    return new Response("method = " + req.method + ", url = " + req.url);
  }

  // Define a property to test behavior of property accessors.
  get nonFunctionProperty() { return {foo: 123}; }

  get functionProperty() { return (a, b) => a - b; }
}

export class MyActor extends DurableObject {
  #counter = 0;

  constructor(ctx, env) {
    super(ctx, env);

    assert.strictEqual(this.ctx, ctx);
    assert.strictEqual(this.env, env);
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
    // Test this.env here just to prove omitting the constructor entirely works.
    return new Response("default service " + this.env.twelve);
  }
}

export let basicServiceBinding = {
  async test(controller, env, ctx) {
    // Test service binding RPC.
    assert.strictEqual(await env.self.oneArg(3), 36);
    assert.strictEqual(await env.self.oneArgOmitCtx(3), 37);
    assert.strictEqual(await env.self.oneArgOmitEnvCtx(3), 6);
    assert.rejects(() => env.self.twoArgs(123, 2),
        "Cannot call handler function \"twoArgs\" over RPC because it has the wrong " +
        "number of arguments. A simple function handler can only be called over RPC if it has " +
        "exactly the arguments (arg, env, ctx), where only the first argument comes from the " +
        "client. To support multi-argument RPC functions, use class-based syntax (extending " +
        "WorkerEntrypoint) instead.");
    assert.rejects(() => env.self.noArgs(),
        "Attempted to call RPC function \"noArgs\" with the wrong number of arguments. " +
        "When calling a top-level handler function that is not declared as part of a class, you " +
        "must always send exactly one argument. In order to support variable numbers of " +
        "arguments, the server must use class-based syntax (extending WorkerEntrypoint) " +
        "instead.");
    assert.rejects(() => env.self.oneArg(1, 2),
        "Attempted to call RPC function \"oneArg\" with the wrong number of arguments. " +
        "When calling a top-level handler function that is not declared as part of a class, you " +
        "must always send exactly one argument. In order to support variable numbers of " +
        "arguments, the server must use class-based syntax (extending WorkerEntrypoint) " +
        "instead.");

    // If we restore multi-arg support, remove the `rejects` checks above and un-comment these:
    // assert.strictEqual(await env.self.noArgs(), 13);
    // assert.strictEqual(await env.self.twoArgs(123, 2), 258);
    // assert.strictEqual(await env.self.twoArgs(123, 2, "foo", "bar", "baz"), 258);
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

    // Properties that return a function can actually be called.
    assert.strictEqual(await env.MyService.functionProperty(19, 6), 13);

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

    // Check we cannot access `env` or `ctx`.
    await assert.rejects(() => getByName("env")(), {
      name: "TypeError",
      message: "The RPC receiver does not implement the method \"env\"."
    });
    await assert.rejects(() => getByName("ctx")(), {
      name: "TypeError",
      message: "The RPC receiver does not implement the method \"ctx\"."
    });

    // Check what happens if we access something that's actually declared as a property on the
    // class. The difference in error message proves to us that `env` and `ctx` weren't visible at
    // all, which is what we want.
    // TODO(soon): When we add pipelining support, also make sure that functions inside `env` and
    //   `ctx` can't be accessed.
    await assert.rejects(() => getByName("nonFunctionProperty")(), {
      name: "TypeError",
      message: "\"nonFunctionProperty\" is not a function."
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
    assert.strictEqual(await resp.text(), "default service 12");
  },
}

export let loopbackJsRpcTarget = {
  async test(controller, env, ctx) {
    let stub = new RpcStub(new MyCounter(4));
    assert.strictEqual(await stub.increment(5), 9);
    assert.strictEqual(await stub.increment(7), 16);
  },
}

export let sendStubOverRpc = {
  async test(controller, env, ctx) {
    let stub = new RpcStub(new MyCounter(4));
    assert.strictEqual(await env.MyService.incrementCounter(stub, 5), 9);
    assert.strictEqual(await stub.increment(7), 16);
  },
}

export let receiveStubOverRpc = {
  async test(controller, env, ctx) {
    let stub = await env.MyService.makeCounter(17);
    assert.strictEqual(await stub.increment(2), 19);
    assert.strictEqual(await stub.increment(-10), 9);
  },
}
