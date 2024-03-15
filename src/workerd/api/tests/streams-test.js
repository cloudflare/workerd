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

export const readAllTextRequestSmall = {
  async test() {
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(enc.encode('hello '));
        c.enqueue(enc.encode('world!'));
        c.close();
      }
    });
    const request = new Request('http://example.org', { method: 'POST', body: rs });
    const text = await request.text();
    strictEqual(text, 'hello world!');
  }
};

export const readAllTextResponseSmall = {
  async test() {
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(enc.encode('hello '));
        c.enqueue(enc.encode('world!'));
        c.close();
      }
    });
    const response = new Response(rs);
    const text = await response.text();
    strictEqual(text, 'hello world!');
  }
};

export const readAllTextRequestBig = {
  async test() {
    const chunks = [
      'a'.repeat(4097),
      'b'.repeat(4097 * 2),
      'c'.repeat(4097 * 4),
    ];
    let check = '';
    const enc = new TextEncoder();

    const rs = new ReadableStream({
      pull(c) {
        if (chunks.length === 0) {
          c.close();
          return;
        }
        const chunk = chunks.shift();
        check += chunk;
        c.enqueue(enc.encode(chunk));
      }
    });
    const request = new Request('http://example.org', { method: 'POST', body: rs });
    const text = await request.text();
    strictEqual(text.length, check.length);
    strictEqual(text, check);
  }
};

export const readAllTextResponseBig = {
  async test() {
    const chunks = [
      'a'.repeat(4097),
      'b'.repeat(4097 * 2),
      'c'.repeat(4097 * 4),
    ];
    let check = '';
    const enc = new TextEncoder();

    const rs = new ReadableStream({
      async pull(c) {
        await scheduler.wait(10);
        if (chunks.length === 0) {
          c.close();
          return;
        }
        const chunk = chunks.shift();
        check += chunk;
        c.enqueue(enc.encode(chunk));
      }
    });
    const response = new Response(rs);
    const promise = response.text();
    const text = await promise;
    strictEqual(text.length, check.length);
    strictEqual(text, check);
  }
};

export const readAllTextFailedPull = {
  async test() {
    const rs = new ReadableStream({
      async pull(c) {
        await scheduler.wait(10);
        throw new Error('boom');
      }
    });
    const response = new Response(rs);
    const promise = response.text();
    try {
      await promise;
      throw new Error('error was expected');
    } catch (err) {
      strictEqual(err.message, 'boom');
    }
  }
};

export const readAllTextFailedStart = {
  async test() {
    const rs = new ReadableStream({
      async start(c) {
        await scheduler.wait(10);
        throw new Error('boom');
      }
    });
    const response = new Response(rs);
    const promise = response.text();
    try {
      await promise;
      throw new Error('error was expected');
    } catch (err) {
      strictEqual(err.message, 'boom');
    }
  }
};

export const readAllTextFailed = {
  async test() {
    const rs = new ReadableStream({
      async start(c) {
        await scheduler.wait(10);
        c.error(new Error('boom'));
      }
    });
    const response = new Response(rs);
    ok(!rs.locked);
    const promise = response.text();
    ok(rs.locked);
    try {
      await promise;
      throw new Error('error was expected');
    } catch (err) {
      strictEqual(err.message, 'boom');
    }
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

export const writableStreamGcTraceFinishes = {
  test() {
    // TODO(soon): We really need better testing for gc visitation.
    const ws = new WritableStream();
    gc();
  }
};

export default {
  async fetch(request, env) {
    strictEqual(request.headers.get('content-length'), '10');
    return new Response(request.body);
  }
};
