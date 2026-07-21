// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import { WorkerEntrypoint, DurableObject, RpcTarget } from 'cloudflare:workers';

export class MyService extends WorkerEntrypoint {
  // Define a property to test behavior of property accessors.
  get nonFunctionProperty() {
    // As a regression test for EW-9282, check that logging in a getter invoked via JSRPC is
    // reflected in traces does not result in missing onset errors. This works through
    // tail-worker-test, which calls into other tests to get traces from them and includes several
    // tail worker implementations. See tail-worker-test.js for test output.
    console.log('foo');
    return { foo: 123 };
  }

  // Test method that returns a transient object with methods.
  // This tests that setJsRpcInfo is only called once for the entrypoint method,
  // not again for transient object methods.
  getCounter() {
    console.log('getCounter called');
    return {
      increment() {
        console.log('increment called on transient');
        return 1;
      },
      getValue() {
        console.log('getValue called on transient');
        return 42;
      },
    };
  }

  constructor(ctx, env) {
    // As a regression test for EW-9282, check that logging in the constructor does not result in
    // missing onset errors. This requires setting the onset event early on, before getting a
    // handler to the entrypoint.
    console.log('bar');
    super(ctx, env);
  }

  async neverReturn() {
    await new Promise((resolve) => {});
  }

  async testDispose(counter) {
    let counterDup = counter.dup();

    return {
      counter: counter.dup(), // need to dup() for return
      async incrementOriginal(n) {
        // This will fail because after the call ends, the counter stub is disposed.
        return await counter.increment(n);
      },
      [Symbol.dispose]() {
        counterDup[Symbol.dispose]();
      },
    };
  }

  async leak(stub) {
    // Leak the input stub
    stub.dup();

    // Return something that contains stubs, holding the context open.
    return {
      noop() {},
    };
  }

  async leakButReturnPlainObject(stub) {
    // Leak the input stub, so it will be disposed when the context is torn down.
    stub.dup();

    // Return a plain object (one with no stubs and no disposer). This should NOT
    // hold the context open, so the stub should be dropped promptly.
    return { foo: 123 };
  }
}

export class MyActor extends DurableObject {
  async functionProperty() {}

  constructor(ctx, env) {
    // As a regression test for EW-9282, check that logging in the constructor does not result in
    // missing onset errors – for DOs, we need to report the onset even earlier.
    super(ctx, env);
    this.ctx.blockConcurrencyWhile(async () => {
      console.log('baz');
    });
  }

  makePostAbortCallTester() {
    return new PostAbortCallTester(this.ctx);
  }
}

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
}

class PostAbortCallTester extends RpcTarget {
  constructor(ctx) {
    super();
    this.ctx = ctx;
  }

  abort() {
    this.ctx.abort('test aborted by abort()');
  }

  async failCriticalSection() {
    await this.ctx.blockConcurrencyWhile(() => {
      throw new Error('test broken critical section');
    });
  }
}

export default {
  async test(controller, env, ctx) {
    let getByName = (name) => {
      return env.MyService.getRpcMethodForTestOnly(name);
    };

    // Check what happens if we access something that's actually declared as a property on the
    // class. The difference in error message proves to us that `env` and `ctx` weren't visible at
    // all, which is what we want.
    await assert.rejects(() => getByName('nonFunctionProperty')(), {
      name: 'TypeError',
      message: '"nonFunctionProperty" is not a function.',
    });

    let id = env.MyActor.idFromName('foo');
    let stub = env.MyActor.get(id);
    await stub.functionProperty();

    // Test transient object methods - this should only report jsrpc.method for the initial
    // getCounter() call, not for the subsequent increment() and getValue() calls on the
    // returned transient object.
    let counter = await env.MyService.getCounter();
    let result1 = await counter.increment();
    let result2 = await counter.getValue();
    assert.strictEqual(result1, 1);
    assert.strictEqual(result2, 42);
  },
};

export let namedServiceBinding = {
  async test(controller, env, ctx) {
    // A stateless entrypoint method that never returns should fail due to PendingEvent tracking.
    await assert.rejects(() => env.MyService.neverReturn(), {
      name: 'Error',
      message:
        "The Workers runtime canceled this request because it detected that your Worker's code " +
        'had hung and would never generate a response. Refer to: ' +
        'https://developers.cloudflare.com/workers/observability/errors/',
    });
  },
};

export let disposal = {
  async test(controller, env, ctx) {
    // V8 fatal error happens in this block
    // A more complex case with testDispose().
    {
      let counter = new MyCounter(3);
      let obj = await env.MyService.testDispose(counter);

      // But after the call, the stub is disposed.
      await assert.rejects(obj.incrementOriginal(), {
        name: 'Error',
        message: 'RPC stub used after being disposed.',
      });

      // But of course, disposing the return value overall breaks everything.
      obj[Symbol.dispose]();

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

// Test that calls to an RpcTarget made after the context is aborted don't get delivered.
export let portAbortCall = {
  async test(controller, env, ctx) {
    {
      let id = env.MyActor.newUniqueId();
      let actor = env.MyActor.get(id);
      let stub = await actor.makePostAbortCallTester();

      let abortPromise = stub.abort();

      await assert.rejects(abortPromise, {
        name: 'Error',
        message: 'test aborted by abort()',
      });
    }

    // Start over with a new stub, this time use failCriticalSection() to break the actor. As of
    // this writing, this differs significantly from plain `abort()` in that
    // `IoContext::abortException` never gets set, since `IoContext::abort()` is not directly
    // called, but instead the exception is joined into the on-abort promise.
    {
      let id = env.MyActor.newUniqueId();
      let actor = env.MyActor.get(id);
      let stub = await actor.makePostAbortCallTester();

      let failPromise = stub.failCriticalSection();

      await assert.rejects(failPromise, {
        name: 'Error',
        message: 'test broken critical section',
      });
    }
  },
};
