import assert from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

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
  },
};
