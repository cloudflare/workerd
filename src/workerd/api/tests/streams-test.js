// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual, ok, deepStrictEqual, rejects, throws } from 'node:assert';

const enc = new TextEncoder();

// A standard WritableStream allocates an AbortSignal for its controller, which must not
// require an active IoContext: constructing one at module scope works.
const moduleScopeWritable = new WritableStream();

export const globalScopeWritableStream = {
  test() {
    ok(moduleScopeWritable instanceof WritableStream);
    strictEqual(moduleScopeWritable.locked, false);
  },
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
      body: readable,
    });
    for await (const _ of resp.body) {
      // intentionally empty
    }
  },
};

// Regression test for https://github.com/cloudflare/workerd/issues/5113
export const tsRequest = {
  async test(ctrl, env) {
    const { readable, writable } = new TransformStream({
      expectedLength: 10,
    });
    const writer = writable.getWriter();
    writer.write(enc.encode('hellohello'));
    writer.close();
    const resp = await env.subrequest.fetch(
      new Request('http://example.org', {
        method: 'POST',
        body: readable,
      })
    );
    for await (const _ of resp.body) {
      // intentionally empty
    }
  },
};

export const byobMin = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
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
  },
};

export const cancelWriteOnReleaseLock = {
  async test() {
    const ws = new WritableStream({
      write() {
        return new Promise(() => {});
      },
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
    strictEqual(
      results[0].reason.message,
      'This WritableStream writer has been released.'
    );
    strictEqual(results[1].status, 'fulfilled');

    // Make sure we can still get another writer
    const _writer2 = ws.getWriter();
  },
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
        },
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
        },
      });
      ok(!cancelCalled);
      await writable.abort('boom');
      ok(cancelCalled);
    }

    {
      const { writable } = new TransformStream({
        async cancel(reason) {
          throw new Error('boomy');
        },
      });
      await rejects(writable.abort('boom'), { message: 'boomy' });
    }
  },
};

export const writableStreamGcTraceFinishes = {
  test() {
    // TODO(soon): We really need better testing for GC visitation.
    const _ws = new WritableStream();
    gc();
  },
};

// TODO(streams-tests): the tests below exercise internal body streams
// (`new Response(...).body`) and standard byte streams, not identity
// streams. They live here until the readable/body suite restructuring
// gives them a permanent home.

// Test BYOB reader constraints
export const byobReaderConstraints = {
  async test() {
    const response = new Response('foo bar');
    const stream = response.body;
    const reader = stream.getReader({ mode: 'byob' });
    // Start a read - this will consume part of the stream
    reader.read(new Uint8Array(32)).catch(() => {}); // Ignore the result

    // We use rejects() with async wrapper instead of throws() because the error
    // is thrown synchronously without streams_enable_constructors but returned as
    // a rejected promise when that flag is enabled. The async wrapper handles both.

    // Cannot BYOB with a zero-length buffer
    await rejects(async () => reader.read(new Uint8Array(0)), TypeError);

    // Cannot BYOB an ArrayBuffer, only an ArrayBufferView
    await rejects(async () => reader.read(new ArrayBuffer(32)), TypeError);

    // Cannot use BYOB reader as a non-BYOB reader
    await rejects(async () => reader.read(), TypeError);
  },
};

// Test non-standard readAtLeast() extension with default reader (should throw)
export const readAtLeastDefaultReaderThrows = {
  async test() {
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        c.enqueue(enc.encode('hello'));
        c.close();
      },
    });

    const reader = rs.getReader();
    throws(() => reader.readAtLeast(1), TypeError);
    reader.releaseLock();

    // Consume the stream to clean up
    for await (const _ of rs) {
      // intentionally empty
    }
  },
};

// Test non-standard readAtLeast() extension with BYOB reader
// Note: The original ew-test expected value=undefined on done, which was the legacy
// behavior of internal streams. With `internal_stream_byob_return_view` compat flag
// (enabled since 2024-05-13), the spec-compliant behavior returns an empty view.
export const readAtLeastByobReader = {
  async test(ctrl, env) {
    // Use service binding to get chunked response
    const response = await env.subrequest.fetch('http://test/chunked');
    const reader = response.body.getReader({ mode: 'byob' });

    // First readAtLeast: request min 4 bytes
    // Server sends: 'foo' (3) + 'bar' (3) = 6 bytes, first chunk 'foo' only 3 bytes
    // so readAtLeast(4) should wait for more data
    let result = await reader.readAtLeast(4, new Uint8Array(20));
    let value = new TextDecoder().decode(result.value);
    strictEqual(result.done, false);
    strictEqual(value.length, 6);
    strictEqual(value, 'foobar');

    // Regular read
    result = await reader.read(new Uint8Array(20));
    value = new TextDecoder().decode(result.value);
    strictEqual(value.length, 1);
    strictEqual(value, 'b');
    strictEqual(result.done, false);

    // Second readAtLeast: request min 4 bytes, only 'az' (2 bytes) remain
    // Server sends: 'a' (1) + 'z' (1) = 2 bytes, then closes
    result = await reader.readAtLeast(4, new Uint8Array(20));
    value = new TextDecoder().decode(result.value);
    strictEqual(value.length, 2);
    strictEqual(value, 'az');
    strictEqual(result.done, false);

    // Final read should be done - spec requires empty view, not undefined
    result = await reader.readAtLeast(4, new Uint8Array(20));
    strictEqual(result.done, true);
    ok(result.value instanceof Uint8Array);
    strictEqual(result.value.byteLength, 0);
  },
};

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    // Endpoint for chunked data for readAtLeast tests
    if (url.pathname === '/chunked') {
      const rs = new ReadableStream({
        type: 'bytes',
        async pull(controller) {
          // Simulate chunked input: foo, bar, b, a, z
          const chunks = [
            enc.encode('foo'),
            enc.encode('bar'),
            enc.encode('b'),
            enc.encode('a'),
            enc.encode('z'),
          ];
          for (const chunk of chunks) {
            controller.enqueue(chunk);
            await scheduler.wait(1);
          }
          controller.close();
        },
      });
      return new Response(rs);
    }

    strictEqual(request.headers.get('content-length'), '10');
    return new Response(request.body);
  },
};

async function createPendingReadableStreamFromRefs() {
  const refs = [];
  for (let i = 0; i < 8; i++) {
    let nextCalled = false;
    const { promise: pending, resolve } = Promise.withResolvers();
    const iterator = {
      resolve,
      next() {
        nextCalled = true;
        return pending;
      },
      [Symbol.asyncIterator]() {
        return this;
      },
    };
    const stream = ReadableStream.from(iterator);
    refs.push(new WeakRef(iterator), new WeakRef(stream));

    const reader = stream.getReader();
    reader.read().catch(() => {});
    await scheduler.wait(0);
    ok(nextCalled, 'the unresolved pull was not started');
    reader.releaseLock();
  }
  return refs;
}

export const readableStreamFromPendingPromiseCollects = {
  async test() {
    const refs = await createPendingReadableStreamFromRefs();
    strictEqual(refs.length, 16);

    for (let i = 0; i < 4; i++) {
      await scheduler.wait(0);
      gc();
    }

    let alive = 0;
    for (const ref of refs) {
      if (ref.deref() !== undefined) alive++;
    }
    ok(
      alive <= 2,
      `expected pending ReadableStream.from cycles to be collected, ` +
        `${alive} of ${refs.length} objects still alive`
    );
  },
};
