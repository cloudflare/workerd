import { strictEqual, ok, deepStrictEqual, rejects, throws } from 'node:assert';

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
        },
      }),
    });
    for await (const _ of resp.body) {
    }
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
    }
  },
};

// Regression test for https://github.com/cloudflare/workerd/issues/5113
export const rsRequest = {
  async test(ctrl, env) {
    const resp = await env.subrequest.fetch(
      new Request('http://example.org', {
        method: 'POST',
        body: new ReadableStream({
          expectedLength: 10,
          start(c) {
            c.enqueue(enc.encode('hellohello'));
            c.close();
          },
        }),
      })
    );
    for await (const _ of resp.body) {
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

export const cancelReadsOnReleaseLock = {
  async test() {
    const rs = new ReadableStream();
    const reader = rs.getReader();
    const read = reader.read();

    const result = await Promise.allSettled([read, reader.releaseLock()]);
    strictEqual(result[0].status, 'rejected');
    strictEqual(
      result[0].reason.message,
      'This ReadableStream reader has been released.'
    );
    strictEqual(result[1].status, 'fulfilled');

    // Make sure we can still get another reader
    const reader2 = rs.getReader();
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
    const writer2 = ws.getWriter();
  },
};

export const readAllTextRequestSmall = {
  async test() {
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(enc.encode('hello '));
        c.enqueue(enc.encode('world!'));
        c.close();
      },
    });
    const request = new Request('http://example.org', {
      method: 'POST',
      body: rs,
    });
    const text = await request.text();
    strictEqual(text, 'hello world!');
  },
};

export const readAllTextResponseSmall = {
  async test() {
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(enc.encode('hello '));
        c.enqueue(enc.encode('world!'));
        c.close();
      },
    });
    const response = new Response(rs);
    const text = await response.text();
    strictEqual(text, 'hello world!');
  },
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
      },
    });
    const request = new Request('http://example.org', {
      method: 'POST',
      body: rs,
    });
    const text = await request.text();
    strictEqual(text.length, check.length);
    strictEqual(text, check);
  },
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
      },
    });
    const response = new Response(rs);
    const promise = response.text();
    const text = await promise;
    strictEqual(text.length, check.length);
    strictEqual(text, check);
  },
};

export const readAllTextFailedPull = {
  async test() {
    const rs = new ReadableStream({
      async pull(c) {
        await scheduler.wait(10);
        throw new Error('boom');
      },
    });
    const response = new Response(rs);
    await rejects(response.text(), { message: 'boom' });
  },
};

export const readAllTextFailedStart = {
  async test() {
    const rs = new ReadableStream({
      async start(c) {
        await scheduler.wait(10);
        throw new Error('boom');
      },
    });
    const response = new Response(rs);
    await rejects(response.text(), { message: 'boom' });
  },
};

export const readAllTextFailed = {
  async test() {
    const rs = new ReadableStream({
      async start(c) {
        await scheduler.wait(10);
        c.error(new Error('boom'));
      },
    });
    const response = new Response(rs);
    ok(!rs.locked);
    const promise = response.text();
    ok(rs.locked);
    await rejects(promise, { message: 'boom' });
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
    const ws = new WritableStream();
    gc();
  },
};

export const readableStreamFromAsyncGenerator = {
  async test() {
    async function* gen() {
      await scheduler.wait(10);
      yield 'hello';
      await scheduler.wait(10);
      yield 'world';
    }
    const rs = ReadableStream.from(gen());
    const chunks = [];
    for await (const chunk of rs) {
      chunks.push(chunk);
    }
    deepStrictEqual(chunks, ['hello', 'world']);
  },
};

export const readableStreamFromSyncGenerator = {
  async test() {
    const rs = ReadableStream.from(['hello', 'world']);
    const chunks = [];
    for await (const chunk of rs) {
      chunks.push(chunk);
    }
    deepStrictEqual(chunks, ['hello', 'world']);
  },
};

export const readableStreamFromSyncGenerator2 = {
  async test() {
    function* gen() {
      yield 'hello';
      yield 'world';
    }
    const rs = ReadableStream.from(gen());
    const chunks = [];
    for await (const chunk of rs) {
      chunks.push(chunk);
    }
    deepStrictEqual(chunks, ['hello', 'world']);
  },
};

export const readableStreamFromAsyncCanceled = {
  async test() {
    async function* gen() {
      let count = 0;
      try {
        count++;
        yield 'hello';
        count++;
        yield 'world';
      } finally {
        strictEqual(count, 1);
      }
    }
    const rs = ReadableStream.from(gen());
    const chunks = [];
    for await (const chunk of rs) {
      chunks.push(chunk);
      return;
    }
    deepStrictEqual(chunks, ['hello']);
  },
};

export const readableStreamFromThrowingAsyncGen = {
  async test() {
    async function* gen() {
      yield 'hello';
      throw new Error('boom');
    }
    const rs = ReadableStream.from(gen());
    const chunks = [];
    async function consumeStream() {
      for await (const chunk of rs) {
        chunks.push(chunk);
      }
    }
    await rejects(consumeStream, { message: 'boom' });
    deepStrictEqual(chunks, ['hello']);
  },
};

export const readableStreamFromNoopAsyncGen = {
  async test() {
    async function* gen() {}
    const rs = ReadableStream.from(gen());
    const chunks = [];
    for await (const chunk of rs) {
      chunks.push(chunk);
    }
    deepStrictEqual(chunks, []);
  },
};

// Tests for ReadableStream.from() cancel behavior per WPT spec
export const readableStreamFromCancelRejectsWhenReturnRejects = {
  async test() {
    const rejectError = new Error('return error');
    const iterable = {
      async next() {
        return { value: undefined, done: true };
      },
      async return() {
        throw rejectError;
      },
      [Symbol.asyncIterator]() {
        return this;
      },
    };

    const rs = ReadableStream.from(iterable);
    const reader = rs.getReader();

    await rejects(reader.cancel(), rejectError);
  },
};

export const readableStreamFromCancelRejectsWhenReturnThrows = {
  async test() {
    const throwError = new Error('return throws');
    const iterable = {
      async next() {
        return { value: undefined, done: true };
      },
      return() {
        throw throwError;
      },
      [Symbol.asyncIterator]() {
        return this;
      },
    };

    const rs = ReadableStream.from(iterable);
    const reader = rs.getReader();

    await rejects(reader.cancel(), (err) => err === throwError);
  },
};

export const readableStreamFromCancelRejectsWhenReturnNotMethod = {
  async test() {
    const iterable = {
      async next() {
        return { value: undefined, done: true };
      },
      return: 42, // exists but not callable
      [Symbol.asyncIterator]() {
        return this;
      },
    };

    const rs = ReadableStream.from(iterable);
    const reader = rs.getReader();

    await rejects(reader.cancel(), {
      name: 'TypeError',
      message: /return/,
    });
  },
};

export const readableStreamFromCancelRejectsWhenReturnNonObject = {
  async test() {
    const iterable = {
      async next() {
        return { value: undefined, done: true };
      },
      async return() {
        return 42; // fulfills with non-object
      },
      [Symbol.asyncIterator]() {
        return this;
      },
    };

    const rs = ReadableStream.from(iterable);
    const reader = rs.getReader();

    await rejects(reader.cancel(), {
      name: 'TypeError',
    });
  },
};

export const readableStreamFromCancelResolvesWhenReturnMissing = {
  async test() {
    const iterable = {
      async next() {
        return { value: undefined, done: true };
      },
      // no return method
      [Symbol.asyncIterator]() {
        return this;
      },
    };

    const rs = ReadableStream.from(iterable);
    const reader = rs.getReader();

    // Should resolve without error when return() is missing
    await Promise.all([reader.cancel(), reader.closed]);
  },
};

export const abortWriterAfterGc = {
  async test() {
    function getWriter() {
      const { writable } = new IdentityTransformStream();
      return writable.getWriter();
    }

    const writer = getWriter();
    gc();
    await writer.abort();
  },
};

export const finalReadOnInternalStreamReturnsBuffer = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    await writer.close();

    const reader = readable.getReader({ mode: 'byob' });
    let result = await reader.read(new Uint8Array(10));
    strictEqual(result.done, true);
    ok(result.value instanceof Uint8Array);
    strictEqual(result.value.byteLength, 0);
    strictEqual(result.value.buffer.byteLength, 10);

    result = await reader.read(new Uint8Array(10));
    strictEqual(result.done, true);
    ok(result.value instanceof Uint8Array);
    strictEqual(result.value.byteLength, 0);
    strictEqual(result.value.buffer.byteLength, 10);
  },
};

// Test that canceling a stream rejects body consume function
export const cancelStreamRejectsBodyConsume = {
  async test() {
    const response = new Response('foo bar');
    const stream = response.body;

    stream.cancel(new Error('a good reason'));

    await rejects(response.text(), TypeError);
  },
};

// Test that canceling a reader resolves closed promise
export const cancelReaderResolvesClosedPromise = {
  async test() {
    const response = new Response('foo bar');
    const stream = response.body;
    const reader = stream.getReader();

    reader.cancel();
    const closed = await reader.closed;
    strictEqual(typeof closed, 'undefined');
    reader.releaseLock();

    await rejects(response.text(), TypeError);
  },
};

// Test that getReader with bad mode throws
export const getReaderBadModeThrows = {
  test() {
    const response = new Response('foo bar');
    const stream = response.body;

    throws(() => stream.getReader({ mode: 'nope' }), TypeError);
  },
};

// Test that stream is locked after getReader() called
export const streamLockedAfterGetReader = {
  test() {
    const response = new Response('foo bar');
    const stream = response.body;

    const reader = stream.getReader();

    ok(stream.locked);

    throws(() => stream.getReader(), TypeError);

    reader.releaseLock();
    ok(!stream.locked);
    reader.releaseLock(); // Second time should be a no-op
  },
};

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

// Test cancel error type propagation
export const cancelErrorTypePropagation = {
  async test() {
    class ExampleError extends Error {
      constructor() {
        super('foo bar');
        this.name = 'ExampleError';
      }
    }

    const cancelErrorTests = [
      {
        cancelWith: new Error('test'),
        expectError: 'Error: test',
      },
      {
        cancelWith: 'test',
        expectError: 'Error: test',
      },
      {
        cancelWith: 'jsg.Error: test',
        expectError: 'Error: jsg.Error: test',
      },
      {
        cancelWith: new TypeError('Problems!'),
        expectError: 'TypeError: Problems!',
        errorType: TypeError,
      },
      {
        cancelWith: new RangeError('Problems!'),
        expectError: 'RangeError: Problems!',
        errorType: RangeError,
      },
      {
        cancelWith: new SyntaxError('The semicolons are bad'),
        expectError: 'SyntaxError: The semicolons are bad',
        errorType: SyntaxError,
      },
      {
        cancelWith: new ReferenceError("Didn't find it"),
        expectError: "ReferenceError: Didn't find it",
        errorType: ReferenceError,
      },
      {
        cancelWith: undefined,
        expectError: 'Error: Stream was cancelled.',
      },
      {
        cancelWith: new ExampleError(),
        expectError: 'Error: ExampleError: foo bar',
        errorType: Error,
      },
    ];

    for (const testCase of cancelErrorTests) {
      const ts = new IdentityTransformStream();

      const writer = ts.writable.getWriter();
      const reader = ts.readable.getReader();
      const writePromise = writer.write(new TextEncoder().encode('a'));
      const writerActualClosed = writer.close();
      await reader.cancel(testCase.cancelWith);

      for (const promise of [writePromise, writerActualClosed]) {
        await rejects(promise, (e) => {
          strictEqual(String(e), testCase.expectError);
          if (testCase.errorType) {
            ok(e instanceof testCase.errorType);
          }
          return true;
        });
      }
    }
  },
};

// Test IdentityTransformStream write before read
export const identityTransformWriteBeforeRead = {
  async test() {
    const MAX_RW = 10;
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();

    const writePromises = [];
    for (let i = 0; i < MAX_RW; i++) {
      writePromises.push(writer.write(new Uint8Array([i])));
    }

    const chunks = [];
    for (let i = 0; i < MAX_RW; i++) {
      chunks.push(await reader.read());
    }

    await Promise.all(writePromises);

    for (let i = 0; i < chunks.length; i++) {
      deepStrictEqual([...chunks[i].value], [i]);
      strictEqual(chunks[i].done, false);
    }

    const writeClosePromise = writer.close();
    const chunk = await reader.read();
    await writeClosePromise;

    strictEqual(chunk.done, true);

    await writer.closed;
    await reader.closed;
  },
};

// Test IdentityTransformStream read before write
export const identityTransformReadBeforeWrite = {
  async test() {
    const MAX_RW = 10;
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();

    // IdentityTransformStream only supports one pending read at a time,
    // so we test read-before-write by starting each read before its write
    const chunks = [];
    for (let i = 0; i < MAX_RW; i++) {
      const readPromise = reader.read();
      await writer.write(new Uint8Array([i]));
      chunks.push(await readPromise);
    }

    for (let i = 0; i < chunks.length; i++) {
      deepStrictEqual([...chunks[i].value], [i]);
      strictEqual(chunks[i].done, false);
    }

    const readClosePromise = reader.read();
    await writer.close();
    const chunk = await readClosePromise;

    strictEqual(chunk.done, true);

    await writer.closed;
    await reader.closed;
  },
};

// Test closed promise under lock release
export const closedPromiseUnderLockRelease = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();

    const writer = writable.getWriter();
    const reader = readable.getReader();

    const writerClosed = writer.closed;
    const readerClosed = reader.closed;

    writer.releaseLock();

    await rejects(writerClosed, TypeError);

    reader.releaseLock();

    await rejects(readerClosed, TypeError);
  },
};

// Test closed promise under writer abort
export const closedPromiseUnderWriterAbort = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();

    const writer = writable.getWriter();
    const reader = readable.getReader();

    const writerClosed = writer.closed;
    const readerClosed = reader.closed;

    const readPromise = reader.read();
    await writer.abort(new Error('Some arbitrary, capricious reason.'));

    await rejects(writerClosed, Error);
    await rejects(readPromise, Error);
    await rejects(readerClosed, Error);
  },
};

// Test FixedLengthStream constructor preconditions
export const fixedLengthStreamPreconditions = {
  test() {
    // Can construct with negative zero
    new FixedLengthStream(-0.0);

    // Can construct with fraction (coerced to 0)
    new FixedLengthStream(0.00001);

    // Can construct with MAX_SAFE_INTEGER
    new FixedLengthStream(Number.MAX_SAFE_INTEGER);

    // Cannot construct with unsafe integer
    throws(() => new FixedLengthStream(Number.MAX_SAFE_INTEGER + 1), TypeError);

    // Cannot construct with negative integer
    throws(() => new FixedLengthStream(-1), TypeError);
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

export const writeSubarray = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();

    const u8 = new Uint8Array([1, 2, 3, 4]);

    const writer = writable.getWriter();
    const reader = readable.getReader();

    writer.write(u8.subarray(1, 3));
    writer.close();

    const { value } = await reader.read();

    strictEqual(value.length, 2);
    strictEqual(value[0], u8[1]);
    strictEqual(value[1], u8[2]);
  },
};

export const writableStreamWriterConstructor = {
  test() {
    const t = new IdentityTransformStream();
    new WritableStreamDefaultWriter(t.writable);
  },
};

export const readableStreamDefaultReaderConstructor = {
  test() {
    const t = new IdentityTransformStream();
    new ReadableStreamDefaultReader(t.readable);
  },
};

export const readableStreamByobReaderConstructor = {
  test() {
    const t = new IdentityTransformStream();
    new ReadableStreamBYOBReader(t.readable);
  },
};

export const byobReaderDetachesBuffer = {
  async test() {
    const ts = new IdentityTransformStream();
    const view = new Uint8Array(10);
    const buffer = view.buffer;
    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader({ mode: 'byob' });
    strictEqual(view.byteLength, 10);
    strictEqual(view.buffer.byteLength, 10);
    const res = await Promise.all([
      writer.write(new Uint8Array(10)),
      reader.read(view),
    ]);

    strictEqual(view.byteLength, 0);
    strictEqual(view.buffer.byteLength, 0);

    ok(res[1].value.buffer instanceof ArrayBuffer);
    ok(res[1].value.buffer !== buffer);

    await rejects(async () => reader.read(view), TypeError);

    // Using a non-detachable ArrayBuffer must fail with a rejection
    const memory = new WebAssembly.Memory({
      initial: 10,
      maximum: 10,
      shared: true,
    });
    await rejects(
      async () => reader.read(new Uint8Array(memory.buffer)),
      TypeError
    );

    await rejects(
      async () => reader.read(new Uint8Array(new SharedArrayBuffer(10))),
      TypeError
    );
  },
};

export const captureSyncThrows = {
  async test() {
    const { readable } = new IdentityTransformStream();
    const reader = readable.getReader({ mode: 'byob' });
    // Without the captureThrowsAsRejections flag enabled, this would throw synchronously.
    // With the flag enabled, however, the synchronous throw is changed into a promise rejection.
    await rejects(async () => reader.read(new ArrayBuffer(10)), TypeError);
  },
};

export const teeFixedLengthStreamNoHang = {
  async test() {
    const ts = new FixedLengthStream(11);
    const writer = ts.writable.getWriter();
    writer.write(new TextEncoder().encode('foo bar baz'));
    writer.close();
    const [left, right] = ts.readable.tee();
    const response = new Response(left);
    strictEqual(await response.text(), 'foo bar baz');
  },
};

export const transformStreamReadAllBytes = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const response = new Response(readable);
    const writer = writable.getWriter();

    const N = 8;
    const M = 5000;

    const writePromise = (async () => {
      for (let i = 0; i < N; i++) {
        const chunk = new Uint8Array(M);
        chunk.fill(i + 1);
        await writer.write(chunk);
      }
      await writer.close();
    })();

    const body = new Uint8Array(await response.arrayBuffer());
    strictEqual(body.byteLength, N * M);
    for (let i = 0; i < body.length; i++) {
      strictEqual(body[i], Math.floor(i / M) + 1);
    }
    await writePromise;
  },
};

export const transformStreamReadAllText = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const response = new Response(readable);
    const writer = writable.getWriter();

    const lowerCaseA = 97;
    const N = 8;
    const M = 5000;

    const writePromise = (async () => {
      for (let i = 0; i < N; i++) {
        const chunk = new Uint8Array(M);
        chunk.fill(i + lowerCaseA);
        await writer.write(chunk);
      }
      await writer.close();
    })();

    const body = await response.text();
    strictEqual(body.length, N * M);
    for (let i = 0; i < N; i++) {
      const expected = String.fromCharCode(i + lowerCaseA).repeat(M);
      strictEqual(body.slice(i * M, i * M + M), expected);
    }
    await writePromise;
  },
};

export const concurrentReadsRejected = {
  async test() {
    const { readable } = new IdentityTransformStream();
    const reader = readable.getReader();
    const p0 = reader.read();
    await rejects(reader.read(), TypeError);
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
