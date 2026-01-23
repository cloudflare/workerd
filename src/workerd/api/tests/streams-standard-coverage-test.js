// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, ok, throws, rejects, deepStrictEqual } from 'node:assert';
import { mock } from 'node:test';

export const readWithPendingClose = {
  async test() {
    let pullCount = 0;

    const rs = new ReadableStream({
      pull(c) {
        pullCount++;
        if (pullCount === 1) {
          c.enqueue('data');
          c.close();
        }
      },
    });

    const reader = rs.getReader();

    const result1 = await reader.read();
    strictEqual(result1.value, 'data');

    const result2 = await reader.read();
    strictEqual(result2.done, true);
  },
};

export const responseTextWithJsStream = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(new TextEncoder().encode('hello '));
        c.enqueue(new TextEncoder().encode('world'));
        c.close();
      },
    });

    const response = new Response(rs);
    const text = await response.text();
    strictEqual(text, 'hello world');
  },
};

export const responseArrayBufferWithJsStream = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(new Uint8Array([1, 2, 3]));
        c.enqueue(new Uint8Array([4, 5, 6]));
        c.close();
      },
    });

    const response = new Response(rs);
    const buffer = await response.arrayBuffer();
    const arr = new Uint8Array(buffer);
    deepStrictEqual([...arr], [1, 2, 3, 4, 5, 6]);
  },
};

export const responseTextWithErroringStream = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(new TextEncoder().encode('partial'));
        c.error(new Error('stream error'));
      },
    });

    const response = new Response(rs);
    await rejects(response.text(), { message: 'stream error' });
  },
};

export const responseTextWithByteStream = {
  async test() {
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        c.enqueue(new TextEncoder().encode('byte stream text'));
        c.close();
      },
    });

    const response = new Response(rs);
    const text = await response.text();
    strictEqual(text, 'byte stream text');
  },
};

export const pipeToBasic = {
  async test() {
    const chunks = [];
    const rs = new ReadableStream({
      start(c) {
        c.enqueue('a');
        c.enqueue('b');
        c.enqueue('c');
        c.close();
      },
    });

    const ws = new WritableStream({
      write(chunk) {
        chunks.push(chunk);
      },
    });

    await rs.pipeTo(ws);
    deepStrictEqual(chunks, ['a', 'b', 'c']);
  },
};

export const pipeToSourceErrorPreventAbort = {
  async test() {
    const abortFn = mock.fn();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue('data');
      },
      pull(c) {
        c.error(new Error('source failed'));
      },
    });

    const ws = new WritableStream({
      write() {},
      abort: abortFn,
    });

    await rejects(rs.pipeTo(ws, { preventAbort: true }), {
      message: 'source failed',
    });
    strictEqual(abortFn.mock.callCount(), 0);
  },
};

export const pipeToSinkErrorPreventCancel = {
  async test() {
    const cancelFn = mock.fn();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue('data');
      },
      cancel: cancelFn,
    });

    const ws = new WritableStream({
      write() {
        throw new Error('sink failed');
      },
    });

    await rejects(rs.pipeTo(ws, { preventCancel: true }), {
      message: 'sink failed',
    });
    strictEqual(cancelFn.mock.callCount(), 0);
  },
};

export const pipeToPreventClose = {
  async test() {
    const closeFn = mock.fn();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue('data');
        c.close();
      },
    });

    const ws = new WritableStream({
      write() {},
      close: closeFn,
    });

    await rs.pipeTo(ws, { preventClose: true });
    strictEqual(closeFn.mock.callCount(), 0);

    const writer = ws.getWriter();
    await writer.write('more');
    await writer.close();
    strictEqual(closeFn.mock.callCount(), 1);
  },
};

export const pipeToWithSignal = {
  async test() {
    const controller = new AbortController();
    let writeCount = 0;

    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(writeCount++);
        if (writeCount > 100) c.close();
      },
    });

    const ws = new WritableStream({
      write(chunk) {
        if (chunk === 2) {
          controller.abort();
        }
      },
    });

    await rejects(rs.pipeTo(ws, { signal: controller.signal }), {
      name: 'AbortError',
    });
  },
};

export const teeErroredStream = {
  async test() {
    const rs = new ReadableStream({
      start(controller) {
        controller.error(new Error('stream errored'));
      },
    });

    const [branch1, branch2] = rs.tee();

    await rejects(branch1.getReader().read(), { message: 'stream errored' });
    await rejects(branch2.getReader().read(), { message: 'stream errored' });
  },
};

export const teeClosedStream = {
  async test() {
    const rs = new ReadableStream({
      start(controller) {
        controller.close();
      },
    });

    const [branch1, branch2] = rs.tee();

    const result1 = await branch1.getReader().read();
    const result2 = await branch2.getReader().read();

    strictEqual(result1.done, true);
    strictEqual(result2.done, true);
  },
};

export const teeErroredByteStream = {
  async test() {
    const rs = new ReadableStream({
      type: 'bytes',
      start(controller) {
        controller.error(new Error('byte stream errored'));
      },
    });

    const [branch1, branch2] = rs.tee();

    await rejects(branch1.getReader().read(), {
      message: 'byte stream errored',
    });
    await rejects(branch2.getReader().read(), {
      message: 'byte stream errored',
    });
  },
};

export const byobReadOnClosedStream = {
  async test() {
    const rs = new ReadableStream({
      type: 'bytes',
      start(controller) {
        controller.close();
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    const inputBuffer = new Uint8Array(10);
    const result = await reader.read(inputBuffer);

    strictEqual(result.done, true);
    ok(result.value instanceof Uint8Array);
    strictEqual(result.value.byteLength, 0);
    strictEqual(inputBuffer.buffer.byteLength, 0);
  },
};

export const byobReadZeroLengthBuffer = {
  async test() {
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        c.enqueue(new Uint8Array([1, 2, 3]));
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    await rejects(reader.read(new Uint8Array(0)), TypeError);
  },
};

export const getDesiredSizeOnClosedStream = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });

    controller.close();
    strictEqual(controller.desiredSize, 0);

    const result = await rs.getReader().read();
    strictEqual(result.done, true);
  },
};

export const getDesiredSizeOnErroredStream = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
        c.error(new Error('test error'));
      },
    });

    strictEqual(controller.desiredSize, null);
    await rejects(async () => rs.getReader().read(), { message: 'test error' });
  },
};

export const canCloseOrEnqueueOnClosedStream = {
  test() {
    let controller;
    new ReadableStream({
      start(c) {
        controller = c;
        c.close();
      },
    });

    throws(() => controller.enqueue('test'), TypeError);
    throws(() => controller.close(), TypeError);
  },
};

export const canCloseOrEnqueueOnErroredStream = {
  test() {
    let controller;
    new ReadableStream({
      start(c) {
        controller = c;
        c.error(new Error('test error'));
      },
    });

    throws(() => controller.enqueue('test'), TypeError);
    throws(() => controller.close(), TypeError);
  },
};

export const hasBackpressureTest = {
  test() {
    let controller;
    new ReadableStream(
      {
        start(c) {
          controller = c;
        },
      },
      { highWaterMark: 1 }
    );

    strictEqual(controller.desiredSize, 1);
    controller.enqueue('a');
    strictEqual(controller.desiredSize, 0);
    controller.enqueue('b');
    strictEqual(controller.desiredSize, -1);
  },
};

export const byteStreamControllerStates = {
  async test() {
    {
      let controller;
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          controller = c;
          c.close();
        },
      });
      strictEqual(controller.desiredSize, 0);
      const result = await rs.getReader().read();
      strictEqual(result.done, true);
    }

    {
      let controller;
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          controller = c;
          c.error(new Error('test'));
        },
      });
      strictEqual(controller.desiredSize, null);
      await rejects(async () => rs.getReader().read(), { message: 'test' });
    }
  },
};

export const writableStreamAbort = {
  async test() {
    const abortFn = mock.fn();
    const ws = new WritableStream({
      abort: abortFn,
    });

    const writer = ws.getWriter();
    await writer.abort('abort reason');

    strictEqual(abortFn.mock.callCount(), 1);
    strictEqual(abortFn.mock.calls[0].arguments[0], 'abort reason');
  },
};

export const writableStreamBackpressure = {
  async test() {
    let resolveWrite;
    const ws = new WritableStream(
      {
        write() {
          const { promise, resolve } = Promise.withResolvers();
          resolveWrite = resolve;
          return promise;
        },
      },
      { highWaterMark: 1 }
    );

    const writer = ws.getWriter();

    strictEqual(writer.desiredSize, 1);
    const writePromise = writer.write('a');
    strictEqual(writer.desiredSize, 0);
    const writePromise2 = writer.write('b');
    strictEqual(writer.desiredSize, -1);

    resolveWrite();
    await writePromise;
    resolveWrite();
    await writePromise2;

    strictEqual(writer.desiredSize, 1);
  },
};

export const writableStreamCloseWhileWriting = {
  async test() {
    let resolveWrite;
    const ws = new WritableStream({
      write() {
        const { promise, resolve } = Promise.withResolvers();
        resolveWrite = resolve;
        return promise;
      },
    });

    const writer = ws.getWriter();
    const writePromise = writer.write('test');
    const closePromise = writer.close();

    resolveWrite();
    await writePromise;
    await closePromise;
  },
};

export const readableStreamFromAsyncGenerator = {
  async test() {
    async function* gen() {
      yield 1;
      yield 2;
      yield 3;
    }

    const rs = ReadableStream.from(gen());
    const reader = rs.getReader();

    strictEqual((await reader.read()).value, 1);
    strictEqual((await reader.read()).value, 2);
    strictEqual((await reader.read()).value, 3);
    strictEqual((await reader.read()).done, true);
  },
};

export const readableStreamFromSyncIterable = {
  async test() {
    const rs = ReadableStream.from([1, 2, 3]);
    const reader = rs.getReader();

    strictEqual((await reader.read()).value, 1);
    strictEqual((await reader.read()).value, 2);
    strictEqual((await reader.read()).value, 3);
    strictEqual((await reader.read()).done, true);
  },
};

export const errorInStartAlgorithm = {
  async test() {
    const rs = new ReadableStream({
      start() {
        throw new Error('start error');
      },
    });

    const reader = rs.getReader();
    await rejects(reader.read(), { message: 'start error' });
  },
};

export const errorInPullAlgorithm = {
  async test() {
    const rs = new ReadableStream({
      pull() {
        throw new Error('pull error');
      },
    });

    const reader = rs.getReader();
    await rejects(reader.read(), { message: 'pull error' });
  },
};

export const errorInCancelAlgorithm = {
  async test() {
    const rs = new ReadableStream({
      cancel() {
        throw new Error('cancel error');
      },
    });

    await rejects(rs.cancel(), { message: 'cancel error' });
  },
};

export const multipleConcurrentReads = {
  async test() {
    let pullCount = 0;
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(++pullCount);
        if (pullCount >= 3) c.close();
      },
    });

    const reader = rs.getReader();

    const [r1, r2, r3, r4] = await Promise.all([
      reader.read(),
      reader.read(),
      reader.read(),
      reader.read(),
    ]);

    strictEqual(r1.value, 1);
    strictEqual(r2.value, 2);
    strictEqual(r3.value, 3);
    strictEqual(r4.done, true);
  },
};

export const forcePullWithByobRequest = {
  async test() {
    const rs = new ReadableStream(
      {
        type: 'bytes',
        pull(c) {
          if (c.byobRequest) {
            const view = c.byobRequest.view;
            view[0] = 42;
            c.byobRequest.respond(1);
          }
        },
      },
      { highWaterMark: 0 }
    );

    const reader = rs.getReader({ mode: 'byob' });
    const result = await reader.read(new Uint8Array(10));

    strictEqual(result.value[0], 42);
    strictEqual(result.value.byteLength, 1);
  },
};

export const cancelClosedStream = {
  async test() {
    const cancelFn = mock.fn();
    const rs = new ReadableStream({
      start(controller) {
        controller.close();
      },
      cancel: cancelFn,
    });

    await rs.cancel();
    strictEqual(cancelFn.mock.callCount(), 0);
  },
};

export const cancelErroredStream = {
  async test() {
    const error = new Error('already errored');
    const rs = new ReadableStream({
      start(controller) {
        controller.error(error);
      },
    });

    await rejects(rs.cancel(), error);
  },
};

export const byobReadSuccess = {
  async test() {
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        c.enqueue(new Uint8Array([1, 2, 3]));
        c.close();
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    const result = await reader.read(new Uint8Array(10));
    strictEqual(result.value.byteLength, 3);
    strictEqual(result.value[0], 1);
  },
};

export const multipleByobReads = {
  async test() {
    let pullCount = 0;
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        pullCount++;
        if (c.byobRequest) {
          const view = c.byobRequest.view;
          new Uint8Array(view.buffer, view.byteOffset, view.byteLength).set([
            pullCount,
          ]);
          c.byobRequest.respond(1);
        } else {
          c.enqueue(new Uint8Array([pullCount]));
        }
        if (pullCount >= 3) {
          c.close();
        }
      },
    });

    const reader = rs.getReader({ mode: 'byob' });

    const r1 = await reader.read(new Uint8Array(5));
    strictEqual(r1.value[0], 1);
    strictEqual(r1.value.byteLength, 1);

    const r2 = await reader.read(new Uint8Array(5));
    strictEqual(r2.value[0], 2);
    strictEqual(r2.value.byteLength, 1);

    const r3 = await reader.read(new Uint8Array(5));
    strictEqual(r3.value[0], 3);
    strictEqual(r3.value.byteLength, 1);

    const r4 = await reader.read(new Uint8Array(5));
    strictEqual(r4.done, true);
  },
};

export const concurrentReadsWithClose = {
  async test() {
    let pullCount = 0;

    const rs = new ReadableStream({
      pull(c) {
        pullCount++;
        if (pullCount === 1) {
          c.enqueue('first');
        } else if (pullCount === 2) {
          c.enqueue('second');
          c.close();
        }
      },
    });

    const reader = rs.getReader();

    const [r1, r2, r3] = await Promise.all([
      reader.read(),
      reader.read(),
      reader.read(),
    ]);

    strictEqual(r1.value, 'first');
    strictEqual(r2.value, 'second');
    strictEqual(r3.done, true);
  },
};

export const concurrentReadsWithError = {
  async test() {
    let pullCount = 0;

    const rs = new ReadableStream({
      pull(c) {
        pullCount++;
        if (pullCount === 1) {
          c.enqueue('first');
        } else {
          c.error(new Error('pull error'));
        }
      },
    });

    const reader = rs.getReader();

    const r1 = await reader.read();
    strictEqual(r1.value, 'first');

    await rejects(reader.read(), { message: 'pull error' });
  },
};

export const pipeToWithMidStreamError = {
  async test() {
    let pullCount = 0;

    const rs = new ReadableStream({
      pull(c) {
        pullCount++;
        if (pullCount <= 2) {
          c.enqueue(new TextEncoder().encode(`chunk${pullCount}`));
        } else {
          c.error(new Error('source error mid-stream'));
        }
      },
    });

    const chunks = [];
    const ws = new WritableStream({
      write(chunk) {
        chunks.push(new TextDecoder().decode(chunk));
      },
    });

    await rejects(rs.pipeTo(ws), { message: 'source error mid-stream' });
    strictEqual(chunks.length, 2);
  },
};

export const pipeToWithSinkError = {
  async test() {
    let writeCount = 0;

    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(new TextEncoder().encode('data'));
      },
    });

    const ws = new WritableStream({
      write(chunk) {
        writeCount++;
        if (writeCount > 2) {
          throw new Error('sink error mid-stream');
        }
      },
    });

    await rejects(rs.pipeTo(ws), { message: 'sink error mid-stream' });
    ok(writeCount >= 2);
  },
};

export const pipeToWithAllPreventOptions = {
  async test() {
    const cancelFn = mock.fn();
    const closeFn = mock.fn();
    const abortFn = mock.fn();

    const rs = new ReadableStream({
      start(c) {
        c.enqueue(new TextEncoder().encode('data'));
        c.close();
      },
      cancel: cancelFn,
    });

    const ws = new WritableStream({
      close: closeFn,
      abort: abortFn,
    });

    await rs.pipeTo(ws, {
      preventClose: true,
      preventAbort: true,
      preventCancel: true,
    });

    strictEqual(cancelFn.mock.callCount(), 0);
    strictEqual(abortFn.mock.callCount(), 0);
    strictEqual(closeFn.mock.callCount(), 0);
  },
};

export const byteStreamWithAutoAllocate = {
  async test() {
    let pullCount = 0;
    const rs = new ReadableStream({
      type: 'bytes',
      autoAllocateChunkSize: 1024,
      pull(c) {
        pullCount++;
        if (c.byobRequest) {
          const view = c.byobRequest.view;
          new Uint8Array(view.buffer, view.byteOffset, 3).set([1, 2, 3]);
          c.byobRequest.respond(3);
        } else {
          c.enqueue(new Uint8Array([1, 2, 3]));
        }
        if (pullCount >= 2) {
          c.close();
        }
      },
    });

    const reader = rs.getReader();
    const r1 = await reader.read();
    ok(r1.value instanceof Uint8Array);
    strictEqual(r1.value.length, 3);

    const r2 = await reader.read();
    ok(r2.value instanceof Uint8Array);
  },
};

// In workerd, autoAllocateChunkSize is set by default (unlike the spec).
// Use the 'no_auto_allocate_readable_byte_streams' compat flag for spec-compliant behavior.
export const byteStreamDefaultReaderNoAutoAllocate = {
  async test() {
    let pullCount = 0;
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        pullCount++;
        c.enqueue(new Uint8Array([pullCount]));
        if (pullCount >= 2) {
          c.close();
        }
      },
    });

    const reader = rs.getReader();
    const r1 = await reader.read();
    strictEqual(r1.value[0], 1);
    strictEqual(r1.value.byteLength, 1);

    const r2 = await reader.read();
    strictEqual(r2.value[0], 2);
    strictEqual(r2.value.byteLength, 1);
  },
};

export const drainingReadMultipleChunks = {
  async test() {
    let pullCount = 0;

    const rs = new ReadableStream({
      pull(c) {
        pullCount++;
        if (pullCount <= 3) {
          c.enqueue(new TextEncoder().encode(`chunk${pullCount}`));
        } else {
          c.close();
        }
      },
    });

    const response = new Response(rs);
    const text = await response.text();
    strictEqual(text, 'chunk1chunk2chunk3');
  },
};

export const byobReaderWithVariousTypes = {
  async test() {
    const makeStream = () =>
      new ReadableStream({
        type: 'bytes',
        start(c) {
          c.enqueue(new Uint8Array([0, 1, 2, 3, 4, 5, 6, 7]));
          c.close();
        },
      });

    {
      const rs = makeStream();
      const reader = rs.getReader({ mode: 'byob' });
      const result = await reader.read(new Uint8Array(4));
      strictEqual(result.value.byteLength, 4);
    }

    {
      const rs = makeStream();
      const reader = rs.getReader({ mode: 'byob' });
      const result = await reader.read(new Uint16Array(2));
      strictEqual(result.value.byteLength, 4);
    }

    {
      const rs = makeStream();
      const reader = rs.getReader({ mode: 'byob' });
      const result = await reader.read(new DataView(new ArrayBuffer(4)));
      strictEqual(result.value.byteLength, 4);
    }
  },
};

export const releaseReaderWhileReadPending = {
  async test() {
    let pullResolver;

    const rs = new ReadableStream({
      pull() {
        const { promise, resolve } = Promise.withResolvers();
        pullResolver = resolve;
        return promise;
      },
    });

    const reader = rs.getReader();
    const readPromise = reader.read();

    reader.releaseLock();

    await rejects(readPromise, TypeError);

    if (pullResolver) pullResolver();
  },
};

export const manySmallChunks = {
  async test() {
    let count = 0;
    const rs = new ReadableStream({
      pull(c) {
        count++;
        c.enqueue(new Uint8Array([count]));
        if (count >= 100) {
          c.close();
        }
      },
    });

    const response = new Response(rs);
    const buffer = await response.arrayBuffer();
    strictEqual(new Uint8Array(buffer).length, 100);
  },
};

export const responseArrayBufferWithStringChunk = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue('this is not bytes');
        c.close();
      },
    });

    const response = new Response(rs);
    await rejects(response.arrayBuffer(), {
      message: 'This ReadableStream did not return bytes.',
    });
  },
};

export const responseTextWithNumberChunk = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(12345);
        c.close();
      },
    });

    const response = new Response(rs);
    await rejects(response.text(), {
      message: 'This ReadableStream did not return bytes.',
    });
  },
};

export const responseArrayBufferWithObjectChunk = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue({ foo: 'bar' });
        c.close();
      },
    });

    const response = new Response(rs);
    await rejects(response.arrayBuffer(), {
      message: 'This ReadableStream did not return bytes.',
    });
  },
};

export const byobReadOnCanceledStream = {
  async test() {
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        c.enqueue(new Uint8Array([1, 2, 3]));
      },
    });

    await rs.cancel('test cancel');

    const reader = rs.getReader({ mode: 'byob' });
    const result = await reader.read(new Uint8Array(10));
    strictEqual(result.done, true);
  },
};

export const defaultReadOnCanceledByteStream = {
  async test() {
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        c.enqueue(new Uint8Array([1, 2, 3]));
      },
    });

    await rs.cancel('test cancel');

    const reader = rs.getReader();
    const result = await reader.read();
    strictEqual(result.done, true);
  },
};
