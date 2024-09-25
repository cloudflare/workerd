import { strictEqual } from 'assert';

export const test = {
  async test(_, env) {
    const resp = await env.subrequest.fetch('http://example.org');
    strictEqual(await resp.text(), 'ok');
  },
};

export default {
  async fetch() {
    // Throwing an error in the error event listener should not be catchable directly
    // but should cause the IoContext to be aborted with the error.
    addEventListener(
      'error',
      () => {
        // This error is not going to be catachable. The best we can do is to log it.
        // Unfortunately, workerd currently does not give us any mechanism to verify
        // that it was logged in the test. Let's make sure the response is handled
        // correctly at least.
        throw new Error('boom (real)');
      },
      { once: true }
    );
    queueMicrotask(() => {
      throw new Error('boom (unused)');
    });
    await scheduler.wait(10);
    return new Response('ok');
  },
};
