import assert from 'node:assert';
import { WorkerEntrypoint, DurableObject } from 'cloudflare:workers';

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
