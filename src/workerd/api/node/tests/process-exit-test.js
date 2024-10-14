import { fail, ok, rejects, strictEqual } from 'assert';

let called = false;

process.exit(9999);

export const test = {
  async test(_, env) {
    // We don't really have a way to verifying that the correct log messages
    // we emitted. For now, we need to manually check the log out, and here
    // we check only that we received an internal error and that no other
    // lines of code ran after the process.exit call.
    await rejects(env.subrequest.fetch('http://example.org'), {
      message: /^The Node.js process.exit/,
    });
    ok(!called);
  },
};

let fooCreateCount = 0;

export const test2 = {
  async test(_, env) {
    // We don't really have a way to verifying that the correct log messages
    // we emitted. For now, we need to manually check the log out, and here
    // we check only that we received an internal error and that no other
    // lines of code ran after the process.exit call.
    {
      const obj = env.foo.get(
        env.foo.idFromName('210bd0cbd803ef7883a1ee9d86cce06f')
      );
      await rejects(obj.fetch('http://example.org'), {
        message: /^The Node.js process.exit/,
      });
      await rejects(obj.fetch('http://example.org'), {
        message: /^The Node.js process.exit/,
      });
      // The durable object should have been created twice.
      strictEqual(fooCreateCount, 2);
    }
  },
};

export default {
  async fetch() {
    try {
      process.exit(123);
    } finally {
      called = true;
    }
  },
};

export class Foo {
  constructor() {
    fooCreateCount++;
  }

  fetch() {
    process.exit(123);
    fail('Should never get here.');
  }
}
