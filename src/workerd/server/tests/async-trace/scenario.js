// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Traced by async-trace-test.sh; check.js asserts on the resulting trace.

export default {
  async fetch(request) {
    if (new URL(request.url).pathname === '/message') {
      // A message sent by env.QUEUE, which delivers it as a request.
      await request.arrayBuffer();
      return Response.json({
        metadata: {
          metrics: {
            backlogCount: 0,
            backlogBytes: 0,
            oldestMessageTimestamp: 0,
          },
        },
      });
    }
    await scheduler.wait(1);
    // Read after the wait, so that the wait's bridge is this context's first.
    if ((await request.text()) !== 'ping') {
      throw new Error('unexpected request body');
    }
    return new Response('hello');
  },

  async test(ctrl, env) {
    await new Promise((resolve) => setTimeout(resolve, 1));
    queueMicrotask(() => {});
    // A JavaScript-backed request body: its pump must not take over the fetch operation.
    const body = new ReadableStream({
      start(controller) {
        controller.enqueue(new TextEncoder().encode('ping'));
        controller.close();
      },
    });
    const response = await env.SELF.fetch('http://scenario/', {
      method: 'POST',
      body,
      duplex: 'half',
    });
    if ((await response.text()) !== 'hello') {
      throw new Error('unexpected response');
    }

    // An internal (KJ-backed) stream: its read and write are binding operations.
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const [read] = await Promise.all([
      reader.read(),
      writer.write(new Uint8Array([1, 2, 3])),
    ]);
    if (read.value?.byteLength !== 3) {
      throw new Error('unexpected read');
    }

    // A queue send: the binding's span is lent to the awaitIo that waits for it.
    await env.QUEUE.send({ hello: 'world' });
  },
};
