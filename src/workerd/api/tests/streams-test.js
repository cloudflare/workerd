import {
  strictEqual,
  ok,
  throws
} from 'node:assert';

const enc = new TextEncoder();

export const rs = {
  async test(ctrl, env) {
    const resp = await env.subrequest.fetch('http://example.org', {
      method: 'POST',
      body: new ReadableStream({
        expectedLength: 10,
        start(c) {
          c.enqueue(enc.encode('hellohello'));
          c.close();
        }
      })
    });
    for await (const _ of resp.body) {}
  }
};

export const ts = {
  async test(ctrl, env) {
    const { readable, writable } = new TransformStream({
      expectedLength: 10,
    });
    const writer = writable.getWriter();
    writer.write(enc.encode('hellohello'));
    writer.close();
    const resp = await env.subrequest.fetch('http://example.org', {
      method: 'POST',
      body: readable
    });
    for await (const _ of resp.body) {}
  }
};

export const byobMin = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) { controller = c; }
    });

    async function handleRead(readable) {
      const reader = rs.getReader({ mode: 'byob' });
      const result = await reader.read(new Uint8Array(10), { min: 10 });
      strictEqual(result.done, false);
      strictEqual(result.value.byteLength, 10);
    }

    async function handlePush(controller) {
      for (let n = 0; n < 10; n++) {
        controller.enqueue(new Uint8Array(1));
        await scheduler.wait(10);
      }
    }

    const results = await Promise.allSettled([
      handleRead(rs),
      handlePush(controller),
    ]);

    strictEqual(results[0].status, 'fulfilled');
    strictEqual(results[1].status, 'fulfilled');
  }
};

export const cancelReadsOnReleaseLock = {
  async test() {
    const rs = new ReadableStream();
    const reader = rs.getReader();
    const read = reader.read();

    const result = await Promise.allSettled([read, reader.releaseLock()]);
    strictEqual(result[0].status, 'rejected');
    strictEqual(result[0].reason.message, 'This ReadableStream reader has been released.');
    strictEqual(result[1].status, 'fulfilled');

    // Make sure we can still get another reader
    const reader2 = rs.getReader();
  }
};

export const cancelWriteOnReleaseLock = {
  async test() {
    const ws = new WritableStream({
      write() {
        return new Promise(() => {});
      }
    });
    const writer = ws.getWriter();
    // This first write is just to start the write queue so that the
    // next write becomes pending in the queue. This first write will
    // never be fulfilled since it is in-progress but the queue will
    // be rejected.
    writer.write('ignored');
    const results = await Promise.allSettled([
      writer.write('hello'),
      writer.releaseLock(),
    ]);
    strictEqual(results[0].status, 'rejected');
    strictEqual(results[0].reason.message, 'This WritableStream writer has been released.');
    strictEqual(results[1].status, 'fulfilled');

    // Make sure we can still get another writer
    const writer2 = ws.getWriter();
  }
};

export const tsCancel = {
  async test() {
    // Verify that a TransformStream's cancel function is called when the
    // readable is canceled or the writable is aborted. Verify also that
    // errors thrown by the cancel function are propagated.
    {
      let cancelCalled = false;
      const { readable } = new TransformStream({
        async cancel(reason) {
          strictEqual(reason, 'boom');
          await scheduler.wait(10);
          cancelCalled = true;
        }
      });
      ok(!cancelCalled);
      await readable.cancel('boom');
      ok(cancelCalled);
    }

    {
      let cancelCalled = false;
      const { writable } = new TransformStream({
        async cancel(reason) {
          strictEqual(reason, 'boom');
          await scheduler.wait(10);
          cancelCalled = true;
        }
      });
      ok(!cancelCalled);
      await writable.abort('boom');
      ok(cancelCalled);
    }

    {
      const { writable } = new TransformStream({
        async cancel(reason) {
          throw new Error('boomy');
        }
      });
      try {
        await writable.abort('boom');
        throw new Error('expected to throw');
      } catch (err) {
        strictEqual(err.message, 'boomy');
      }
    }
  }
};

export default {
  async fetch(request, env) {
    strictEqual(request.headers.get('content-length'), '10');
    return new Response(request.body);
  }
};
