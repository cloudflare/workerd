// Test should not fail with a memory leak reported by ASAN.
// Failures here would only occur in an ASAN build.

export const memoryLeak = {
  async test(_ctrl, env) {
    {
      (await env.subrequest.fetch('http://example.org')).body.pipeTo(
        new WritableStream()
      );
    }
    globalThis.gc();
    globalThis.gc();
  },
};

export const memoryLeak2 = {
  async test(_ctrl, env) {
    {
      new ReadableStream().pipeTo(new WritableStream());
    }
    globalThis.gc();
    globalThis.gc();
  },
};

export const memoryLeak3 = {
  async test(_ctrl, env) {
    {
      new ReadableStream().pipeTo(new IdentityTransformStream().writable);
    }
    globalThis.gc();
    globalThis.gc();
  },
};

export default {
  async fetch() {
    await scheduler.wait(1_000);
    return new Response('ok');
  },
};
