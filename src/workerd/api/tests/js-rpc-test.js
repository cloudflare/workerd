import assert from 'node:assert';
import {
  WorkerEntrypoint,
  DurableObject,
  RpcStub,
  RpcTarget,
} from 'cloudflare:workers';

class MyCounter extends RpcTarget {
  constructor(i = 0) {
    super();
    this.i = i;
  }

  async increment(j) {
    this.i += j;
    return this.i;
  }

  disposed = false;

  #disposedResolver;
  #onDisposedPromise = new Promise(
    (resolve) => (this.#disposedResolver = resolve)
  );

  [Symbol.dispose]() {
    this.disposed = true;
    this.#disposedResolver();
  }

  onDisposed() {
    return Promise.race([
      this.#onDisposedPromise,
      scheduler.wait(1000).then(() => {
        throw new Error('timed out waiting for disposal');
      }),
    ]);
  }

  // Tests that `fetch()` is not special for RpcTargets.
  async fetch(a, b, c) {
    return `${this.i} ${a} ${b} ${c}`;
  }
}

class NonRpcClass {
  foo() {
    return 123;
  }
  get bar() {
    return {
      baz() {
        return 456;
      },
    };
  }

  i = 0;
  increment(i) {
    this.i += i;
  }
}

export let nonClass = {
  async noArgs(x, env, ctx) {
    assert.strictEqual(typeof ctx.waitUntil, 'function');
    return x === undefined ? env.twelve + 1 : 'param not undefined?';
  },

  async oneArg(i, env, ctx) {
    assert.strictEqual(typeof ctx.waitUntil, 'function');
    return env.twelve * i;
  },

  async oneArgOmitCtx(i, env) {
    return env.twelve * i + 1;
  },

  async oneArgOmitEnvCtx(i) {
    return 2 * i;
  },

  async twoArgs(i, j, env, ctx) {
    assert.strictEqual(typeof ctx.waitUntil, 'function');
    return i * j + env.twelve;
  },

  async fetch(req, env, ctx) {
    // This is used in the stream test to fetch some gziped data.
    if (req.url.endsWith('/gzip')) {
      return new Response('this text was gzipped', {
        headers: {
          'Content-Encoding': 'gzip',
        },
      });
    } else if (req.url.endsWith('/stream-from-rpc')) {
      let stream = await env.MyService.returnReadableStream();
      return new Response(stream);
    } else {
      throw new Error('unknown route');
    }
  },
};

// Globals used to test passing RPC promises or properties across I/O contexts (which is expected
// to fail).
let globalRpcPromise;

// Promise initialized by testWaitUntil() and then resolved shortly later, in a waitUntil task.
let globalWaitUntilPromise;

export class MyService extends WorkerEntrypoint {
  constructor(ctx, env) {
    super(ctx, env);

    assert.strictEqual(this.ctx, ctx);
    assert.strictEqual(this.env, env);

    // This shouldn't be callable!
    this.instanceMethod = () => {
      return 'nope';
    };

    this.instanceObject = {
      func: (a) => a * 5,
    };
  }

  async noArgsMethod(x) {
    return x === undefined ? this.env.twelve + 1 : 'param not undefined?';
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

  async getAnObject(i) {
    return { foo: 123 + i, counter: new MyCounter(i) };
  }

  async getMap() {
    let map = new Map();
    map.set('foo', 123);
    map.set('bar', 456);
    return map;
  }

  async fetch(req, x) {
    assert.strictEqual(x, undefined);
    return new Response('method = ' + req.method + ', url = ' + req.url);
  }

  // Define a property to test behavior of property accessors.
  get nonFunctionProperty() {
    return { foo: 123 };
  }

  get functionProperty() {
    return (a, b) => a - b;
  }

  get objectProperty() {
    let nullPrototype = { foo: 123 };
    nullPrototype.__proto__ = null;

    return {
      func: (a, b) => a * b,
      deeper: {
        deepFunc: (a, b) => a / b,
      },
      counter5: new MyCounter(5),
      nonRpc: new NonRpcClass(),
      nullPrototype,
      someText: 'hello',
    };
  }

  get promiseProperty() {
    return scheduler.wait(10).then(() => 123);
  }

  get rejectingPromiseProperty() {
    return Promise.reject(new Error('REJECTED'));
  }

  get throwingProperty() {
    throw new Error('PROPERTY THREW');
  }

  throwingMethod() {
    throw new Error('METHOD THREW');
  }

  async tryUseGlobalRpcPromise() {
    return await globalRpcPromise;
  }
  async tryUseGlobalRpcPromisePipeline() {
    return await globalRpcPromise.increment(1);
  }

  async getNonRpcClass() {
    return { obj: new NonRpcClass() };
  }

  async getNullPrototypeObject() {
    let obj = { foo: 123 };
    obj.__proto__ = null;
    return obj;
  }

  async getFunction() {
    let func = (a, b) => a ^ b;
    func.someProperty = 123;
    return func;
  }

  getRpcPromise(callback) {
    return callback();
  }
  getNestedRpcPromise(callback) {
    return { value: callback() };
  }
  getRemoteNestedRpcPromise(callback) {
    // Use a function as a cheap way to return a JsRpcStub that has a remote property `value` which
    // itself is initialized as a JsRpcPromise.
    let result = () => {};
    result.value = callback();
    return result;
  }
  getRpcProperty(callback) {
    return callback.foo;
  }
  getNestedRpcProperty(callback) {
    return { value: callback.foo };
  }
  getRemoteNestedRpcProperty(callback) {
    // Use a function as a cheap way to return a JsRpcStub that has a remote property `value` which
    // itself is initialized as a JsRpcProperty.
    let result = () => {};

    // We have to dup() the callback because otherwise it will be implicitly disposed when this
    // function returns, but we want it to remain accessible as a property of the returned
    // function.
    let dupCallback = callback.dup();
    result.value = dupCallback.foo;

    // Not really
    result[Symbol.dispose] = () => dupCallback[Symbol.dispose]();

    return result;
  }

  get(a) {
    return a + 1;
  }
  put(a, b) {
    return a + b;
  }
  delete(a) {
    return a - 1;
  }

  async testDispose(counter) {
    // Prove that the counter works at this point.
    let count = await counter.increment(5);

    let counterDup = counter.dup();

    return {
      count,
      counter: counter.dup(), // need to dup() for return
      async incrementOriginal(n) {
        // This will fail because after the call ends, the counter stub is disposed.
        return await counter.increment(n);
      },
      async incrementDup(n) {
        // This will succeed since we kept a dup.
        return await counterDup.increment(n);
      },
      [Symbol.dispose]() {
        counterDup[Symbol.dispose]();
      },
    };
  }

  async leak(stub) {
    // Leak the input stub
    stub.dup();

    let ctx = this.ctx;

    // Return something that contains stubs, holding the context open.
    return {
      noop() {},
      abort() {
        ctx.abort(new RangeError('foo bar abort reason'));
      },
    };
  }

  async leakButReturnPlainObject(stub) {
    // Leak the input stub, so it will be disposed when the context is torn down.
    stub.dup();

    // Return a plain object (one with no stubs and no disposer). This should NOT
    // hold the context open, so the stub should be dropped promptly.
    return { foo: 123 };
  }

  async writeToStream(stream) {
    let writer = stream.getWriter();
    let enc = new TextEncoder();
    await writer.write(enc.encode('foo, '));
    await writer.write(enc.encode('bar, '));
    await writer.write(enc.encode('baz!'));
    await writer.close();
  }

  async writeToStreamExpectingError(stream) {
    // We expect the writes to fail on the remote end. However, that won't happen
    // right away. Due to backpressure and flow control, write does not wait until
    // the remote end has received the data. It resolves as soon as there is space
    // in the buffer to perform another write. Here we'll just keep writing until
    // we get the expected error.
    let writer = stream.getWriter();
    let enc = new TextEncoder();
    for (;;) {
      await writer.write(enc.encode('foo, '));
    }
  }

  async writeToStreamAbort(stream) {
    // In this test, aborting the stream should propagate back to the remote
    // side, causing the stream to be errored and the abort algorithm to be
    // called with the provided error. Unfortunately the current implementation
    // does not allow for that. The reason passed to abort is cached locally and
    // is never communicated to the remote. Instead, the remote side will end up
    // with a generic disconnect error. Sad face.
    let writer = stream.getWriter();
    writer.abort(new Error('boom'));
  }

  async readFromStream(stream) {
    return await new Response(stream).text();
  }

  async returnReadableStream() {
    let { readable, writable } = new IdentityTransformStream();
    this.ctx.waitUntil(this.writeToStream(writable));
    return readable;
  }

  async returnMultipleReadableStreams() {
    let { readable, writable } = new IdentityTransformStream();
    this.ctx.waitUntil(this.writeToStream(writable));

    let pair2 = new IdentityTransformStream();
    this.ctx.waitUntil(this.writeToStream(pair2.writable));
    let readable2 = pair2.readable;

    return [readable, readable2];
  }

  async roundTrip(value) {
    return value;
  }

  async returnEmptyHeaders() {
    return new Headers();
  }

  async returnHeaders() {
    let result = new Headers();
    result.append('foo', 'bar');
    result.append('Set-Cookie', 'abc');
    result.append('set-cookie', 'def');
    result.append('corge', '!@#');
    result.append('Content-Length', '123');
    return result;
  }

  async returnRequest() {
    return new Request('http://my-url.com', {
      method: 'PUT',
      headers: {
        'Accept-Encoding': 'bazzip',
        Foo: 'Bar',
      },
      redirect: 'manual',
      body: 'Hello every body!',
      cf: {
        abc: 123,
        hello: 'goodbye',
      },
    });
  }

  async returnResponse() {
    return new Response('Response body!', {
      status: 404,
      headers: {
        'Content-Type': 'abc',
      },
      cf: { foo: 123, bar: 'def' },
    });
  }

  testWaitUntil() {
    // Initialize globalWaitUntilPromise to a promise that will be resolved in a waitUntil task
    // later on. We'll perform a cross-context wait to verify that the waitUntil task actually
    // completes and resolves the promise.
    let resolve;
    globalWaitUntilPromise = new Promise((r) => {
      resolve = r;
    });

    this.ctx.waitUntil(
      (async () => {
        await scheduler.wait(100);
        resolve();
      })()
    );
  }
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
    return new Response('from ActorNoExtends');
  }

  // This can't be called!
  async foo() {
    return 123;
  }
}

export default class DefaultService extends WorkerEntrypoint {
  async fetch(req) {
    // Test this.env here just to prove omitting the constructor entirely works.
    return new Response('default service ' + this.env.twelve);
  }
}

export let basicServiceBinding = {
  async test(controller, env, ctx) {
    // Test service binding RPC.
    assert.strictEqual(await env.self.oneArg(3), 36);
    assert.strictEqual(await env.self.oneArgOmitCtx(3), 37);
    assert.strictEqual(await env.self.oneArgOmitEnvCtx(3), 6);
    await assert.rejects(() => env.self.twoArgs(123, 2), {
      name: 'TypeError',
      message:
        'Cannot call handler function "twoArgs" over RPC because it has the wrong ' +
        'number of arguments. A simple function handler can only be called over RPC if it has ' +
        'exactly the arguments (arg, env, ctx), where only the first argument comes from the ' +
        'client. To support multi-argument RPC functions, use class-based syntax (extending ' +
        'WorkerEntrypoint) instead.',
    });
    await assert.rejects(() => env.self.noArgs(), {
      name: 'TypeError',
      message:
        'Attempted to call RPC function "noArgs" with the wrong number of arguments. ' +
        'When calling a top-level handler function that is not declared as part of a class, you ' +
        'must always send exactly one argument. In order to support variable numbers of ' +
        'arguments, the server must use class-based syntax (extending WorkerEntrypoint) ' +
        'instead.',
    });
    await assert.rejects(() => env.self.oneArg(1, 2), {
      name: 'TypeError',
      message:
        'Attempted to call RPC function "oneArg" with the wrong number of arguments. ' +
        'When calling a top-level handler function that is not declared as part of a class, you ' +
        'must always send exactly one argument. In order to support variable numbers of ' +
        'arguments, the server must use class-based syntax (extending WorkerEntrypoint) ' +
        'instead.',
    });

    // If we restore multi-arg support, remove the `rejects` checks above and un-comment these:
    // assert.strictEqual(await env.self.noArgs(), 13);
    // assert.strictEqual(await env.self.twoArgs(123, 2), 258);
    // assert.strictEqual(await env.self.twoArgs(123, 2, "foo", "bar", "baz"), 258);
  },
};

export let extendingEntrypointClasses = {
  async test(controller, env, ctx) {
    // Verify that we can instantiate classes that inherit built-in classes.
    let svc = new MyService(ctx, env);
    assert.equal(svc instanceof WorkerEntrypoint, true);
  },
};

export let namedServiceBinding = {
  async test(controller, env, ctx) {
    assert.strictEqual(await env.MyService.noArgsMethod(), 13);
    assert.strictEqual(await env.MyService.oneArgMethod(3), 36);
    assert.strictEqual(await env.MyService.twoArgsMethod(123, 2), 258);

    // Properties that return a function can actually be called.
    assert.strictEqual(await env.MyService.functionProperty(19, 6), 13);

    // Members of an object-typed property can be invoked.
    assert.strictEqual(await env.MyService.objectProperty.func(6, 4), 24);
    assert.strictEqual(
      await env.MyService.objectProperty.deeper.deepFunc(6, 3),
      2
    );
    assert.strictEqual(
      await env.MyService.objectProperty.counter5.increment(3),
      8
    );

    // Awaiting a property itself gets the value.
    assert.strictEqual(
      JSON.stringify(await env.MyService.nonFunctionProperty),
      '{"foo":123}'
    );
    assert.strictEqual(await env.MyService.objectProperty.someText, 'hello');
    {
      let counter = await env.MyService.objectProperty.counter5;
      assert.strictEqual(await counter.increment(3), 8);
      assert.strictEqual(await counter.increment(7), 15);
    }

    {
      let func = await env.MyService.objectProperty.func;
      assert.strictEqual(await func(3, 7), 21);
    }
    {
      let func = await env.MyService.getFunction();
      assert.strictEqual(await func(3, 6), 5);
      assert.strictEqual(await func.someProperty, 123);
    }
    {
      // Pipeline the function call.
      let func = env.MyService.getFunction();
      assert.strictEqual(await func(3, 6), 5);
      assert.strictEqual(await func.someProperty, 123);
    }

    // A property that returns a Promise will wait for the Promise.
    assert.strictEqual(await env.MyService.promiseProperty, 123);

    let sawFinally = false;
    assert.strictEqual(
      await env.MyService.promiseProperty.finally(() => {
        sawFinally = true;
      }),
      123
    );
    assert.strictEqual(sawFinally, true);

    // `fetch()` is special, the params get converted into a Request.
    let resp = await env.MyService.fetch('http://foo/', { method: 'POST' });
    assert.strictEqual(await resp.text(), 'method = POST, url = http://foo/');

    await assert.rejects(() => env.MyService.instanceMethod(), {
      name: 'TypeError',
      message:
        'The RPC receiver does not implement the method "instanceMethod".',
    });

    await assert.rejects(() => env.MyService.instanceObject.func(3), {
      name: 'TypeError',
      message:
        'The RPC receiver does not implement the method "instanceObject".',
    });

    await assert.rejects(() => env.MyService.instanceObject, {
      name: 'TypeError',
      message:
        'The RPC receiver does not implement the method "instanceObject".',
    });

    await assert.rejects(() => env.MyService.throwingProperty, {
      name: 'Error',
      message: 'PROPERTY THREW',
    });
    await assert.rejects(() => env.MyService.throwingMethod(), {
      name: 'Error',
      message: 'METHOD THREW',
    });

    await assert.rejects(() => env.MyService.rejectingPromiseProperty, {
      name: 'Error',
      message: 'REJECTED',
    });
    assert.strictEqual(
      await env.MyService.rejectingPromiseProperty.catch((err) => {
        assert.strictEqual(err.message, 'REJECTED');
        return 234;
      }),
      234
    );
    assert.strictEqual(
      await env.MyService.rejectingPromiseProperty.then(
        () => 432,
        (err) => {
          assert.strictEqual(err.message, 'REJECTED');
          return 234;
        }
      ),
      234
    );

    let getByName = (name) => {
      return env.MyService.getRpcMethodForTestOnly(name);
    };

    // Check getRpcMethodForTestOnly() actually works.
    assert.strictEqual(await getByName('twoArgsMethod')(2, 3), 18);

    // Check we cannot call reserved methods.
    await assert.rejects(() => getByName('constructor')(), {
      name: 'TypeError',
      message:
        "'constructor' is a reserved method and cannot be called over RPC.",
    });
    await assert.rejects(() => getByName('fetch')(), {
      name: 'TypeError',
      message: "'fetch' is a reserved method and cannot be called over RPC.",
    });

    // Check we cannot call methods of Object.
    await assert.rejects(() => getByName('toString')(), {
      name: 'TypeError',
      message: 'The RPC receiver does not implement the method "toString".',
    });
    await assert.rejects(() => getByName('hasOwnProperty')(), {
      name: 'TypeError',
      message:
        'The RPC receiver does not implement the method "hasOwnProperty".',
    });

    // Check we cannot access `env` or `ctx`.
    await assert.rejects(() => getByName('env')(), {
      name: 'TypeError',
      message: 'The RPC receiver does not implement the method "env".',
    });
    await assert.rejects(() => getByName('ctx')(), {
      name: 'TypeError',
      message: 'The RPC receiver does not implement the method "ctx".',
    });

    // Check what happens if we access something that's actually declared as a property on the
    // class. The difference in error message proves to us that `env` and `ctx` weren't visible at
    // all, which is what we want.
    await assert.rejects(() => getByName('nonFunctionProperty')(), {
      name: 'TypeError',
      message: '"nonFunctionProperty" is not a function.',
    });
    await assert.rejects(() => getByName('nonFunctionProperty').foo(), {
      name: 'TypeError',
      message: '"nonFunctionProperty.foo" is not a function.',
    });

    // Check that we can't access contents of a property that is a class but not derived from
    // RpcTarget.
    await assert.rejects(() => env.MyService.objectProperty.nonRpc.foo(), {
      name: 'TypeError',
      message: 'The RPC receiver does not implement the method "nonRpc".',
    });
    await assert.rejects(() => env.MyService.objectProperty.nonRpc.bar.baz(), {
      name: 'TypeError',
      message: 'The RPC receiver does not implement the method "nonRpc".',
    });
    await assert.rejects(() => env.MyService.objectProperty.nullPrototype.foo, {
      name: 'TypeError',
      message:
        'The RPC receiver does not implement the method "nullPrototype".',
    });

    // Extra-paranoid check that we can't access methods on env or ctx.
    await assert.rejects(
      () => env.MyService.objectProperty.env.MyService.noArgsMethod(),
      {
        name: 'TypeError',
        message: 'The RPC receiver does not implement the method "env".',
      }
    );
    await assert.rejects(() => env.MyService.objectProperty.ctx.waitUntil(), {
      name: 'TypeError',
      message: 'The RPC receiver does not implement the method "ctx".',
    });

    // Can't serialize instances of classes that aren't derived from RpcTarget.
    await assert.rejects(() => env.MyService.getNonRpcClass(), {
      name: 'DataCloneError',
      message:
        'Could not serialize object of type "NonRpcClass". This type does not support ' +
        'serialization.',
    });
    await assert.rejects(() => env.MyService.getNullPrototypeObject(), {
      name: 'DataCloneError',
      message:
        'Could not serialize object of type "Object". This type does not support ' +
        'serialization.',
    });

    {
      let map = await env.MyService.getMap();
      assert.strictEqual(map.get('foo'), 123);
      assert.strictEqual(map.get('bar'), 456);
      assert.strictEqual(map.get('baz'), undefined);
    }
  },
};

export let namedActorBinding = {
  async test(controller, env, ctx) {
    let id = env.MyActor.idFromName('foo');
    let stub = env.MyActor.get(id);

    assert.strictEqual(await stub.increment(5), 5);
    assert.strictEqual(await stub.increment(2), 7);
    assert.strictEqual(await stub.increment(8), 15);
  },
};

// Test that if the actor class doesn't extend `DurableObject`, we don't allow RPC.
export let actorWithoutExtendsRejectsRpc = {
  async test(controller, env, ctx) {
    let id = env.ActorNoExtends.idFromName('foo');
    let stub = env.ActorNoExtends.get(id);

    // fetch() works.
    let resp = await stub.fetch('http://foo');
    assert.strictEqual(await resp.text(), 'from ActorNoExtends');

    // RPC does not.
    await assert.rejects(() => stub.foo(), {
      name: 'TypeError',
      message:
        'The receiving Durable Object does not support RPC, because its class was not declared ' +
        'with `extends DurableObject`. In order to enable RPC, make sure your class ' +
        'extends the special class `DurableObject`, which can be imported from the module ' +
        '"cloudflare:workers".',
    });
  },
};

// Test calling the default export when it is a class.
export let defaultExportClass = {
  async test(controller, env, ctx) {
    let resp = await env.defaultExport.fetch('http://foo');
    assert.strictEqual(await resp.text(), 'default service 12');
  },
};

export let loopbackJsRpcTarget = {
  async test(controller, env, ctx) {
    let counter = new MyCounter(4);
    let stub = new RpcStub(counter);
    assert.strictEqual(await stub.increment(5), 9);
    assert.strictEqual(await stub.increment(7), 16);

    assert.strictEqual(await stub.fetch(true, 123, 'baz'), '16 true 123 baz');

    assert.strictEqual(counter.disposed, false);
    stub[Symbol.dispose]();

    await assert.rejects(stub.increment(2), {
      name: 'Error',
      message: 'RPC stub used after being disposed.',
    });

    await counter.onDisposed();
    assert.strictEqual(counter.disposed, true);
  },
};

export let sendStubOverRpc = {
  async test(controller, env, ctx) {
    let stub = new RpcStub(new MyCounter(4));
    let stubDup = stub.dup();

    assert.strictEqual(await env.MyService.incrementCounter(stub, 5), 9);

    await assert.rejects(() => stub.increment(7), {
      name: 'Error',
      message: 'RPC stub used after being disposed.',
    });

    assert.strictEqual(await stubDup.increment(7), 16);
  },
};

export let receiveStubOverRpc = {
  async test(controller, env, ctx) {
    let stub = await env.MyService.makeCounter(17);
    assert.strictEqual(await stub.increment(2), 19);
    assert.strictEqual(await stub.increment(-10), 9);

    // Do multiple concurrent calls, they should be delivered in the order in which they were made.
    let promise1 = stub.increment(6);
    let promise2 = stub.increment(4);
    let promise3 = stub.increment(3);
    assert.deepEqual(
      await Promise.all([promise1, promise2, promise3]),
      [15, 19, 22]
    );
  },
};

export let promisePipelining = {
  async test(controller, env, ctx) {
    assert.strictEqual(await env.MyService.makeCounter(12).increment(3), 15);

    assert.strictEqual(await env.MyService.getAnObject(5).foo, 128);
    assert.strictEqual(
      await env.MyService.getAnObject(5).counter.increment(7),
      12
    );

    assert.rejects(() => env.MyService.oneArgMethod(5).foo(), {
      name: 'TypeError',
      message: 'The RPC receiver does not implement the method "foo".',
    });

    assert.rejects(() => env.MyService.getMap().foo(), {
      name: 'TypeError',
      message: 'The RPC receiver does not implement the method "foo".',
    });
  },
};

export let disposal = {
  async test(controller, env, ctx) {
    // Call function that returns plain stub. Dispose it.
    {
      let counter = await env.MyService.makeCounter(12);
      assert.strictEqual(await counter.increment(3), 15);
      counter[Symbol.dispose]();
      await assert.rejects(counter.increment(2), {
        name: 'Error',
        message: 'RPC stub used after being disposed.',
      });
    }

    // Call function that returns an object containing a stub. The object has a dispose() method
    // added that disposes everything inside.
    {
      let obj = await env.MyService.getAnObject(5);
      assert.strictEqual(await obj.counter.increment(7), 12);

      obj[Symbol.dispose]();
      await assert.rejects(obj.counter.increment(2), {
        name: 'Error',
        message: 'RPC stub used after being disposed.',
      });
    }

    // Verify a capability passed as an RPC param receives the disposal callback.
    {
      let counter = new MyCounter(3);
      assert.strictEqual(await env.MyService.incrementCounter(counter, 5), 8);
      await counter.onDisposed();
      assert.strictEqual(counter.disposed, true);
    }

    // A more complex case with testDispose().
    {
      let counter = new MyCounter(3);
      let obj = await env.MyService.testDispose(counter);

      // The counter was able to be incremented during the call.
      assert.strictEqual(obj.count, 8);

      // But after the call, the stub is disposed.
      await assert.rejects(obj.incrementOriginal(), {
        name: 'Error',
        message: 'RPC stub used after being disposed.',
      });

      // The duplicate we created in the call still works though.
      assert.strictEqual(await obj.incrementDup(7), 15);

      // Also, the returned copy works.
      assert.strictEqual(await obj.counter.increment(4), 19);

      // But of course, disposing the return value overall breaks everything.
      obj[Symbol.dispose]();
      await assert.rejects(obj.counter.increment(2), {
        name: 'Error',
        message: 'RPC stub used after being disposed.',
      });
      await assert.rejects(obj.incrementDup(7), {
        name: 'Error',
        message: 'RPC stub used after being disposed.',
      });

      await counter.onDisposed();
      assert.strictEqual(counter.disposed, true);
    }

    // Test a leak situation.
    {
      let counter = new MyCounter(3);
      let obj = await env.MyService.leak(counter);

      // Give a chance for disposal to happen.
      await obj.noop();
      await scheduler.wait(10);

      // It should not have happened.
      assert.strictEqual(counter.disposed, false);

      // Even if we GC the leaked stub, still no disposal!
      gc();
      await scheduler.wait(10);
      assert.strictEqual(counter.disposed, false);

      // If we abort the server's I/O context, though, then the counter is disposed.
      await assert.rejects(obj.abort(), {
        name: 'RangeError',
        message: 'foo bar abort reason',
      });

      await counter.onDisposed();
      assert.strictEqual(counter.disposed, true);
    }

    // Test that a call which returns a plain object does not need to be disposed.
    // Historically, the callee context would not be torn down promptly.
    {
      let counter = new MyCounter(3);
      await env.MyService.leakButReturnPlainObject(counter);

      // Give a chance for disposal to happen. When running with a real socket, this involves
      // non-deterministic scheduling, and it can be relatively slower when running under ASAN,
      // QEMU, etc. so we'll try to dynamically adjust how much time we wait, up to 10s.
      for (let i = 0; i < 1000; i++) {
        if (counter.disposed) break;
        await scheduler.wait(10);
      }

      // It should have happened! A call that returns a plain object should NOT
      // require disposal to clean up its context!
      assert.strictEqual(counter.disposed, true);
    }
  },
};

export let crossContextSharingDoesntWork = {
  async test(controller, env, ctx) {
    // Test what happens if a JsRpcPromise or JsRpcProperty is shared cross-context. This is not
    // intended to work in general, but there are specific cases where it does work, and we should
    // avoid breaking those with future changes.

    // Sharing an RPC promise between contexts works as long as the promise returns a simple value
    // (with no I/O objects), since JsRpcPromise wraps a simple JS promise and we support sharing
    // JS promises.
    globalRpcPromise = env.MyService.oneArgMethod(2);
    assert.strictEqual(await env.MyService.tryUseGlobalRpcPromise(), 24);

    // Sharing a property of a service binding works, because the service  binding itself is not
    // tied to an I/O context. Awaiting the property actually initiates a new RPC session from
    // whatever context performed teh await.
    globalRpcPromise = env.MyService.nonFunctionProperty;
    assert.strictEqual(
      JSON.stringify(await env.MyService.tryUseGlobalRpcPromise()),
      '{"foo":123}'
    );

    // OK, now let's look at cases that do NOT work. These all produce the same error.
    let expectedError = {
      name: 'Error',
      message:
        'Cannot perform I/O on behalf of a different request. I/O objects (such as streams, ' +
        'request/response bodies, and others) created in the context of one request handler ' +
        "cannot be accessed from a different request's handler. This is a limitation of " +
        'Cloudflare Workers which allows us to improve overall performance.',
    };

    // A promise which resolves to a value that contains a stub. The stub cannot be used from a
    // different context.
    //
    // Note that the part that actually fails here is not awaiting the promise, but rather when
    // tryUseGlobalRpcPromise() tries to return the result, it tries to serialize the stub, but
    // it can't do that from the wrong context.
    globalRpcPromise = env.MyService.makeCounter(12);

    await assert.rejects(() => env.MyService.tryUseGlobalRpcPromise(), {
      name: 'Error',
      message:
        'Cannot perform I/O on behalf of a different request. I/O objects (such as streams, ' +
        'request/response bodies, and others) created in the context of one request handler ' +
        "cannot be accessed from a different request's handler. This is a limitation of " +
        'Cloudflare Workers which allows us to improve overall performance. (I/O type: Client)',
    });

    // Pipelining on someone else's promise straight-up doesn't work.
    await assert.rejects(() => env.MyService.tryUseGlobalRpcPromisePipeline(), {
      name: 'Error',
      message:
        'Cannot perform I/O on behalf of a different request. I/O objects (such as streams, ' +
        'request/response bodies, and others) created in the context of one request handler ' +
        "cannot be accessed from a different request's handler. This is a limitation of " +
        'Cloudflare Workers which allows us to improve overall performance. ' +
        '(I/O type: JsRpcPromise)',
    });

    // Now let's try accessing a JsRpcProperty, where the property is NOT a direct property of a
    // top-level service binding. This works even less than a JsRpcPromise, since there's no inner
    // JS promise, it tries to create one on-demand, which fails because the parent object is
    // tied to the original I/O context.
    globalRpcPromise = env.MyService.getAnObject(5).counter;

    await assert.rejects(() => env.MyService.tryUseGlobalRpcPromise(), {
      name: 'Error',
      message:
        'Cannot perform I/O on behalf of a different request. I/O objects (such as streams, ' +
        'request/response bodies, and others) created in the context of one request handler ' +
        "cannot be accessed from a different request's handler. This is a limitation of " +
        'Cloudflare Workers which allows us to improve overall performance. (I/O type: Pipeline)',
    });

    await assert.rejects(() => env.MyService.tryUseGlobalRpcPromisePipeline(), {
      name: 'Error',
      message:
        'Cannot perform I/O on behalf of a different request. I/O objects (such as streams, ' +
        'request/response bodies, and others) created in the context of one request handler ' +
        "cannot be accessed from a different request's handler. This is a limitation of " +
        'Cloudflare Workers which allows us to improve overall performance. ' +
        '(I/O type: JsRpcPromise)',
    });
  },
};

export let waitUntilWorks = {
  async test(controller, env, ctx) {
    globalWaitUntilPromise = null;
    await env.MyService.testWaitUntil();

    assert.strictEqual(globalWaitUntilPromise instanceof Promise, true);
    await globalWaitUntilPromise;
  },
};

export let serializeRpcPromiseOrProprety = {
  async test(controller, env, ctx) {
    // What happens if we actually try to serialize a JsRpcPromise or JsRpcProperty? Let's make
    // sure these aren't, for instance, treated as functions because they are callable.

    let func = () => {
      return { x: 123 };
    };
    func.foo = { x: 456 };

    // If we directly return returning a JsRpcPromise, the system automatically awaits it on the
    // server side because it's a thenable.
    assert.deepEqual(await env.MyService.getRpcPromise(func), {
      x: 123,
    });

    // Pipelining also works.
    assert.strictEqual(await env.MyService.getRpcPromise(func).x, 123);

    // If a JsRpcPromise appears somewhere in the serialization tree, it'll just fail serialization.
    // NOTE: We could choose to make this work later.
    await assert.rejects(() => env.MyService.getNestedRpcPromise(func), {
      name: 'DataCloneError',
      message:
        'Could not serialize object of type "RpcPromise". This type does not support ' +
        'serialization.',
    });
    await assert.rejects(() => env.MyService.getNestedRpcPromise(func).value, {
      name: 'DataCloneError',
      message:
        'Could not serialize object of type "RpcPromise". This type does not support ' +
        'serialization.',
    });
    await assert.rejects(
      () => env.MyService.getNestedRpcPromise(func).value.x,
      {
        name: 'DataCloneError',
        message:
          'Could not serialize object of type "RpcPromise". This type does not support ' +
          'serialization.',
      }
    );

    // Things get a little weird when we return a stub which itself has properties that reflect
    // our RPC promise. If we await fetch the JsRpcPromise itself, this works, again because
    // somewhere along the line V8 says "oh look a thenable" and awaits it, before it can be
    // subject to serialization. That's fine.
    assert.deepEqual(
      await env.MyService.getRemoteNestedRpcPromise(func).value,
      { x: 123 }
    );
    await assert.rejects(
      () => env.MyService.getRemoteNestedRpcPromise(func).value.x,
      {
        name: 'TypeError',
        message: 'The RPC receiver does not implement the method "value".',
      }
    );

    // The story is similar for a JsRpcProperty -- though the implementation details differ.
    assert.deepEqual(await env.MyService.getRpcProperty(func), {
      x: 456,
    });
    assert.strictEqual(await env.MyService.getRpcProperty(func).x, 456);
    await assert.rejects(() => env.MyService.getNestedRpcProperty(func), {
      name: 'DataCloneError',
      message:
        'Could not serialize object of type "RpcProperty". This type does not support ' +
        'serialization.',
    });
    await assert.rejects(() => env.MyService.getNestedRpcProperty(func).value, {
      name: 'DataCloneError',
      message:
        'Could not serialize object of type "RpcProperty". This type does not support ' +
        'serialization.',
    });
    await assert.rejects(
      () => env.MyService.getNestedRpcProperty(func).value.x,
      {
        name: 'DataCloneError',
        message:
          'Could not serialize object of type "RpcProperty". This type does not support ' +
          'serialization.',
      }
    );

    assert.deepEqual(
      await env.MyService.getRemoteNestedRpcProperty(func).value,
      { x: 456 }
    );
    await assert.rejects(
      () => env.MyService.getRemoteNestedRpcProperty(func).value.x,
      {
        name: 'TypeError',
        message: 'The RPC receiver does not implement the method "value".',
      }
    );
    await assert.rejects(
      () => env.MyService.getRemoteNestedRpcProperty(func).value(),
      {
        name: 'TypeError',
        message: '"value" is not a function.',
      }
    );
  },
};

export let streams = {
  async test(controller, env, ctx) {
    // Send WritableStream.
    {
      let { readable, writable } = new IdentityTransformStream();
      let promise = env.MyService.writeToStream(writable);
      let text = await new Response(readable).text();
      assert.strictEqual(text, 'foo, bar, baz!');
      await promise;
    }

    {
      const dec = new TextDecoder();
      let result = '';
      const { promise, resolve } = Promise.withResolvers();
      const writable = new WritableStream({
        write(chunk) {
          result += dec.decode(chunk, { stream: true });
        },
        close() {
          result += dec.decode();
          resolve();
        },
      });
      const p1 = env.MyService.writeToStream(writable);
      await promise;
      assert.strictEqual(result, 'foo, bar, baz!');
      await p1;
    }

    {
      // In this test, the remote side writes a chunk to the stream below, which throws
      // an error. Ideally the error would propagate back to the calling side so that the
      // remote knows the stream failed and can no longer be written to. The call to
      // writeToStreamExpectingError should throw because the error should be propagated
      // through the round trip.
      const dec = new TextDecoder();
      let result = '';
      let writeCalled = 0;
      const writable = new WritableStream({
        write(chunk) {
          writeCalled++;
          throw new Error('boom');
        },
      });

      try {
        await env.MyService.writeToStreamExpectingError(writable);
        throw new Error('should have thrown');
      } catch (err) {
        assert.strictEqual(err.message, 'boom');
        // The write method should have been called once.
        assert.strictEqual(writeCalled, 1);
      }
    }

    {
      // In this test, the remote side aborts the writable stream it receives.
      // The abort should propagate such that the abort algorithm is called and the
      // writeToStreamAbort call should succeed. The error passed on to abort(reason)
      // should be the error that was given on the remote side when abort is called,
      // but we currently do not propagate the abort reason through. What ends up
      // happening is that the local stream is dropped with a generic cancelation
      // error.
      const dec = new TextDecoder();
      const { promise, resolve } = Promise.withResolvers();
      const writable = new WritableStream({
        write(chunk) {},
        abort(reason) {
          resolve(reason);
        },
      });
      await env.MyService.writeToStreamAbort(writable);
      const reason = await promise;
      // TODO(someday): The reason should be the error that was passed to abort on the
      // remote side, but we currently do not propagate this. We end up with a generic
      // disconnection error instead, which certainly not ideal.
      assert.strictEqual(
        reason.message,
        'WritableStream received over RPC was disconnected because the remote execution ' +
          'context has endeded.'
      );
    }

    // TODO(someday): Is there any way to construct an encoded WritableStream? Only system
    //   streams can be encoded, but there's no API that returns an encoded WritableStream I think.

    // Send ReadableStream.
    {
      let { readable, writable } = new IdentityTransformStream();
      let promise = env.MyService.readFromStream(readable);

      let writer = writable.getWriter();
      let enc = new TextEncoder();
      await writer.write(enc.encode('foo, '));
      await writer.write(enc.encode('bar, '));
      await writer.write(enc.encode('baz!'));
      await writer.close();

      assert.strictEqual(await promise, 'foo, bar, baz!');
    }

    // Send a JS-backed ReadableStream.
    {
      let controller;
      let readable = new ReadableStream({
        start(c) {
          controller = c;
        },
      });
      let promise = env.MyService.readFromStream(readable);

      let enc = new TextEncoder();
      controller.enqueue(enc.encode('foo, '));
      controller.enqueue(enc.encode('bar, '));
      controller.enqueue(enc.encode('baz!'));
      controller.close();

      assert.strictEqual(await promise, 'foo, bar, baz!');
    }

    // Send streams that are locked.
    {
      let { readable, writable } = new IdentityTransformStream();

      let writer = writable.getWriter();
      let enc = new TextEncoder();
      writer.write(enc.encode('foo'));

      let reader = readable.getReader();

      assert.rejects(env.MyService.writeToStream(writable), {
        name: 'TypeError',
        message: 'The WritableStream has been locked to a writer.',
      });
      assert.rejects(env.MyService.readFromStream(readable), {
        name: 'TypeError',
        message: 'The ReadableStream has been locked to a reader.',
      });

      // Verify the streams still work.
      let dec = new TextDecoder();
      assert.strictEqual(dec.decode((await reader.read()).value), 'foo');

      writer.write(enc.encode('bar'));
      assert.strictEqual(dec.decode((await reader.read()).value), 'bar');

      writer.close();
      assert.strictEqual((await reader.read()).done, true);
    }

    // Receive ReadableStream.
    {
      let readable = await env.MyService.returnReadableStream();
      let text = await new Response(readable).text();
      assert.strictEqual(text, 'foo, bar, baz!');
    }

    // Receive multiple ReadableStreams.
    {
      let readables = await env.MyService.returnMultipleReadableStreams();
      assert.strictEqual(
        await new Response(readables[0]).text(),
        'foo, bar, baz!'
      );
      assert.strictEqual(
        await new Response(readables[1]).text(),
        'foo, bar, baz!'
      );
    }

    // Send ReadableStream, but fail to fully write it.
    {
      let { readable, writable } = new IdentityTransformStream();
      let promise = env.MyService.readFromStream(readable);

      let writer = writable.getWriter();
      let enc = new TextEncoder();
      await writer.write(enc.encode('foo, '));
      await writer.write(enc.encode('bar, '));
      await writer.write(enc.encode('baz!'));
      await writer.abort('foo');

      await assert.rejects(promise, {
        name: 'Error',
        // TODO(someday): Propagate the actual error.
        message: 'ReadableStream received over RPC disconnected prematurely.',
      });
    }

    // Send fixed-length ReadableStream.
    {
      let { readable, writable } = new FixedLengthStream(
        'foo, bar, baz!'.length
      );
      let promise = env.MyService.readFromStream(readable);

      let writer = writable.getWriter();
      let enc = new TextEncoder();
      await writer.write(enc.encode('foo, '));
      await writer.write(enc.encode('bar, '));
      await writer.write(enc.encode('baz!'));
      await writer.close();

      assert.strictEqual(await promise, 'foo, bar, baz!');
    }

    // Send an encoded ReadableStream
    {
      let gzippedResp = await env.self.fetch('http://foo/gzip');

      let text = await env.MyService.readFromStream(gzippedResp.body);

      assert.strictEqual(text, 'this text was gzipped');
    }

    // Round trip streams.
    {
      let { readable, writable } = new IdentityTransformStream();

      readable = await env.MyService.roundTrip(readable);
      writable = await env.MyService.roundTrip(writable);

      let readPromise = new Response(readable).text();

      let writer = writable.getWriter();
      let enc = new TextEncoder();
      await writer.write(enc.encode('foo, '));
      await writer.write(enc.encode('bar, '));
      await writer.write(enc.encode('baz!'));
      await writer.close();

      assert.strictEqual(await readPromise, 'foo, bar, baz!');
    }

    // Perform an HTTP request whose response uses a ReadableStream obtained over RPC.
    {
      let resp = await env.self.fetch('http://foo/stream-from-rpc');

      assert.strictEqual(await resp.text(), 'foo, bar, baz!');
    }
  },
};

export let serializeHttpTypes = {
  async test(controller, env, ctx) {
    {
      let headers = await env.MyService.returnEmptyHeaders();
      assert.deepEqual([...headers], []);
    }

    {
      let headers = await env.MyService.returnHeaders();
      assert.strictEqual(headers instanceof Headers, true);

      // Awkwardly, there's actually no API to get the non-lowercased header names.
      assert.deepEqual(
        [...headers],
        [
          ['content-length', '123'],
          ['corge', '!@#'],
          ['foo', 'bar'],
          ['set-cookie', 'abc'],
          ['set-cookie', 'def'],
        ]
      );

      assert.deepEqual(headers.getSetCookie(), ['abc', 'def']);
    }

    {
      let req = await env.MyService.returnRequest();

      assert.strictEqual(req.url, 'http://my-url.com');
      assert.strictEqual(req.method, 'PUT');
      assert.strictEqual(req.headers.get('Accept-Encoding'), 'bazzip');
      assert.strictEqual(req.headers.get('Foo'), 'Bar');
      assert.strictEqual(req.redirect, 'manual');

      assert.strictEqual(await req.text(), 'Hello every body!');

      assert.deepEqual(req.cf, {
        abc: 123,
        hello: 'goodbye',
      });
    }

    // Check that a Request with an AbortSignal can't be sent. (We should fix this someday, by
    // making AbortSignal itself RPC-compatible.)
    await assert.rejects(
      env.MyService.roundTrip(
        new Request('http://foo', { signal: AbortSignal.timeout(100) })
      ),
      {
        name: 'DataCloneError',
        message:
          'Could not serialize object of type "AbortSignal". This type does not support ' +
          'serialization.',
      }
    );

    {
      let req = await env.MyService.returnResponse();

      assert.strictEqual(req.status, 404);
      assert.strictEqual(req.statusText, 'Not Found');
      assert.strictEqual(req.headers.get('Content-Type'), 'abc');
      assert.deepEqual(req.cf, { foo: 123, bar: 'def' });

      assert.strictEqual(await req.text(), 'Response body!');
    }
  },
};

// Test that exceptions thrown from async native functions have a proper stack trace. (This is
// not specific to RPC but RPC is a convenient place to test it since we can easily define the
// callee to throw an exception.)
//
// Note that it's only a *local* stack trace of the client-side stack leading up to the call. The
// stack on the server side is not, at present, transmitted to the client.
export let testAsyncStackTrace = {
  async test(controller, env, ctx) {
    try {
      await env.MyService.throwingMethod();
    } catch (e) {
      // verify stack trace was produced
      assert.strictEqual(e.stack.includes('at async Object.test'), true);
    }
  },
};

// Test that exceptions thrown over RPC have the .remote property.
export let testExceptionProperties = {
  async test(controller, env, ctx) {
    try {
      await env.MyService.throwingMethod();
    } catch (e) {
      assert.strictEqual(e.remote, true);
      assert.strictEqual(e.message, 'METHOD THREW');
    }
  },
};

// Test that get(), put(), and delete() are valid RPC method names, not hijacked by Fetcher.
export let canUseGetPutDelete = {
  async test(controller, env, ctx) {
    assert.strictEqual(await env.MyService.get(12), 13);
    assert.strictEqual(await env.MyService.put(5, 7), 12);
    assert.strictEqual(await env.MyService.delete(3), 2);
  },
};

// Test that stubs can still be used after logging them.
export let logging = {
  async test(controller, env, ctx) {
    let counter = new MyCounter(0);
    let stub = new RpcStub(counter);
    assert.strictEqual(await stub.increment(1), 1);
    assert.strictEqual(await stub.increment(1), 2);
    console.log(stub);
    assert.strictEqual(await stub.increment(1), 3);
  },
};

// DOMException is structured cloneable
export let domExceptionClone = {
  test() {
    const de1 = new DOMException('hello', 'NotAllowedError');

    // custom own properties on the instance are not preserved...
    de1.foo = 'ignored';

    const de2 = structuredClone(de1);
    assert.strictEqual(de1.name, de2.name);
    assert.strictEqual(de1.message, de2.message);
    assert.strictEqual(de1.stack, de2.stack);
    assert.strictEqual(de1.code, de2.code);
    assert.notStrictEqual(de1, de2);
    assert.notStrictEqual(de1.foo, de2.foo);
    assert.strictEqual(de2.foo, undefined);
  },
};

export let proxiedRpcTarget = {
  async test(controller, env, ctx) {
    // Proxy RPC target.
    {
      let counter = new MyCounter(0);
      let proxy = new Proxy(counter, {
        get(target, prop, receiver) {
          if (prop == 'increment') {
            return (i) => target.increment(i + 123);
          } else {
            let result = target[prop];
            if (result instanceof Function) {
              result = result.bind(target);
            }
            return result;
          }
        },
      });

      await env.MyService.incrementCounter(proxy, 1);

      assert.strictEqual(counter.i, 124);
    }

    // Proxy plain object.
    {
      let counter = {
        i: 0,
        increment(i) {
          this.i += i;
        },
      };

      let proxy = new Proxy(counter, {
        get(target, prop, receiver) {
          if (prop == 'increment') {
            return (i) => target.increment(i + 123);
          } else {
            let result = target[prop];
            if (result instanceof Function) {
              result = result.bind(target);
            }
            return result;
          }
        },
      });

      await env.MyService.incrementCounter(proxy, 1);

      assert.strictEqual(counter.i, 124);
    }

    // Can't proxy a class that doesn't extend `RpcTarget`.
    {
      let nonRpc = new NonRpcClass();
      let proxy = new Proxy(nonRpc, {
        get(target, prop, receiver) {
          if (prop == 'increment') {
            return (i) => target.increment(i + 123);
          } else {
            let result = target[prop];
            if (result instanceof Function) {
              result = result.bind(target);
            }
            return result;
          }
        },
      });

      await assert.rejects(() => env.MyService.incrementCounter(proxy, 1), {
        name: 'DataCloneError',
        message:
          'Proxy could not be serialized because it is not a valid RPC receiver type. The ' +
          'Proxy must emulate either a plain object or an RpcTarget, as indicated by the ' +
          "Proxy's prototype chain.",
      });
    }

    // Can't proxy an RpcTarget if we've overridden the prototype to say it's something else.
    {
      let counter = new MyCounter(0);
      let proxy = new Proxy(counter, {
        getPrototypeOf(target) {
          return NonRpcClass.prototype;
        },
      });

      await assert.rejects(() => env.MyService.incrementCounter(proxy, 1), {
        name: 'DataCloneError',
        message:
          'Proxy could not be serialized because it is not a valid RPC receiver type. The ' +
          'Proxy must emulate either a plain object or an RpcTarget, as indicated by the ' +
          "Proxy's prototype chain.",
      });
    }

    // CAN proxy a class that doesn't extend `RpcTarget` if we fake the prototype.
    {
      let nonRpc = new NonRpcClass();
      let proxy = new Proxy(nonRpc, {
        getPrototypeOf(target) {
          return RpcTarget.prototype;
        },
      });

      await env.MyService.incrementCounter(proxy, 321);
      assert.strictEqual(nonRpc.i, 321);
    }
  },
};
