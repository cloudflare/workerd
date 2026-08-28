// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual, ok, rejects } from 'node:assert';

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

export default {
  async fetch(request, env) {
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
