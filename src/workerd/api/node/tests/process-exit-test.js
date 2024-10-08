import { rejects, ok } from 'assert';

let called = false;

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

export default {
  async fetch() {
    try {
      process.exit(123);
    } finally {
      called = true;
    }
  },
};
