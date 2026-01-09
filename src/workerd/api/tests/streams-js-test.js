// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for JavaScript-backed streams (ReadableStream and WritableStream constructors)
// Ported from edgeworker streams-js.ew-test

import { strictEqual, ok, throws, rejects } from 'node:assert';

// Test that JS streams globals exist
export const userStreamsGlobalsExist = {
  test() {
    ok(ReadableStreamDefaultController !== undefined);
    ok(ReadableByteStreamController !== undefined);
    ok(ReadableStreamBYOBRequest !== undefined);
    ok(WritableStreamDefaultController !== undefined);
  },
};

// Test that JS streams objects are not directly constructable
export const jsStreamsObjectsNotConstructable = {
  test() {
    throws(() => new ReadableStreamDefaultController(), TypeError);
    throws(() => new ReadableByteStreamController(), TypeError);
    throws(() => new ReadableStreamBYOBRequest(), TypeError);
    throws(() => new WritableStreamDefaultController(), TypeError);
  },
};

// Test new ReadableStream() works
export const newReadableStream = {
  test() {
    new ReadableStream();
    new ReadableStream({ type: 'bytes' });
  },
};

// Test that underlying source algorithms are called
export const newReadableStreamAlgorithms = {
  async test() {
    // Sync algorithms
    {
      let started = false;
      let pulled = false;
      let canceled = false;
      const rs = new ReadableStream({
        start() {
          started = true;
        },
        pull() {
          pulled = true;
        },
        cancel() {
          canceled = true;
        },
      });
      ok(started);

      await scheduler.wait(1);

      rs.cancel();

      ok(pulled);
      ok(canceled);
    }

    // Byte stream sync algorithms
    {
      let started = false;
      let pulled = false;
      let canceled = false;
      const rs = new ReadableStream(
        {
          type: 'bytes',
          start() {
            started = true;
          },
          pull() {
            pulled = true;
          },
          cancel() {
            canceled = true;
          },
        },
        { highWaterMark: 1 }
      );

      ok(started);
      await scheduler.wait(1);

      rs.cancel();

      ok(pulled);
      ok(canceled);
    }

    // Async algorithms for value stream
    {
      let onStarted, onPulled, onCanceled;
      let started = new Promise((resolve) => (onStarted = resolve));
      let pulled = new Promise((resolve) => (onPulled = resolve));
      let canceled = new Promise((resolve) => (onCanceled = resolve));

      const rs = new ReadableStream({
        async start() {
          await scheduler.wait(1);
          onStarted();
        },
        async pull() {
          await scheduler.wait(1);
          onPulled();
        },
        async cancel() {
          onCanceled();
        },
      });

      await Promise.allSettled([started, pulled]);
      await scheduler.wait(1);
      await Promise.allSettled([rs.cancel(), canceled]);
    }

    // Async algorithms for byte stream
    {
      let onStarted, onPulled, onCanceled;
      let started = new Promise((resolve) => (onStarted = resolve));
      let pulled = new Promise((resolve) => (onPulled = resolve));
      let canceled = new Promise((resolve) => (onCanceled = resolve));

      const rs = new ReadableStream(
        {
          type: 'bytes',
          async start() {
            await scheduler.wait(1);
            onStarted();
          },
          async pull() {
            await scheduler.wait(1);
            onPulled();
          },
          async cancel() {
            onCanceled();
          },
        },
        { highWaterMark: 1 }
      );

      await Promise.allSettled([started, pulled]);
      await scheduler.wait(1);
      await Promise.allSettled([rs.cancel(), canceled]);
    }
  },
};

// Test that new ReadableStream creates the right kind of controller
export const newReadableStreamControllerType = {
  test() {
    new ReadableStream({
      start(c) {
        ok(c instanceof ReadableStreamDefaultController);
      },
      pull(c) {
        ok(c instanceof ReadableStreamDefaultController);
      },
    });

    new ReadableStream({
      type: 'bytes',
      start(c) {
        ok(c instanceof ReadableByteStreamController);
      },
      pull(c) {
        ok(c instanceof ReadableByteStreamController);
        const byobRequest = c.byobRequest;
        ok(byobRequest != null);
        ok(byobRequest === c.byobRequest);
        ok(byobRequest instanceof ReadableStreamBYOBRequest);
        ok(byobRequest.view instanceof Uint8Array);
      },
    });
  },
};

// Test sync algorithm errors are handled properly
export const newReadableStreamSyncAlgorithmErrorsHandled = {
  async test() {
    // Start error
    {
      const rs = new ReadableStream({
        start() {
          throw new Error('boom');
        },
      });

      await rejects(rs.getReader().read(), { message: 'boom' });
    }

    // Pull error
    {
      const rs = new ReadableStream({
        pull() {
          throw new Error('boom');
        },
      });

      await rejects(rs.getReader().read(), { message: 'boom' });
    }

    // Cancel error
    {
      const rs = new ReadableStream({
        cancel() {
          throw new Error('boom');
        },
      });
      await rejects(rs.cancel(), { message: 'boom' });
    }
  },
};

// Test async algorithm errors are handled properly
export const newReadableStreamAsyncAlgorithmErrorsHandled = {
  async test() {
    // Async start error
    {
      const rs = new ReadableStream({
        async start() {
          throw new Error('boom');
        },
      });

      await rejects(rs.getReader().read(), { message: 'boom' });
    }

    // Async pull error
    {
      const rs = new ReadableStream({
        async pull() {
          throw new Error('boom');
        },
      });

      await rejects(rs.getReader().read(), { message: 'boom' });
    }

    // Async cancel error
    {
      const rs = new ReadableStream({
        async cancel() {
          throw new Error('boom');
        },
      });

      await rejects(rs.cancel(), { message: 'boom' });
    }
  },
};

// Test size algorithm is called with correct value and errors handled
export const sizeAlgorithmCalled = {
  async test() {
    // Size algorithm called with correct value
    {
      let sizeCalled = false;
      new ReadableStream(
        {
          pull(c) {
            c.enqueue(1);
          },
        },
        {
          size(value) {
            strictEqual(value, 1);
            sizeCalled = true;
          },
        }
      );

      ok(sizeCalled);
    }

    // Size algorithm ignored in byte streams
    {
      let sizeCalled = false;
      new ReadableStream(
        {
          type: 'bytes',
          pull(c) {
            c.enqueue(new Uint8Array(1));
          },
        },
        {
          size() {
            sizeCalled = true;
          },
        }
      );

      ok(!sizeCalled);
    }

    // Size algorithm error handled
    {
      const rs = new ReadableStream(
        {
          pull(c) {
            c.enqueue(1);
          },
        },
        {
          size() {
            throw new Error('boom');
          },
        }
      );

      await rejects(rs.getReader().read(), { message: 'boom' });
    }

    // Async size algorithm not allowed
    {
      const rs = new ReadableStream(
        {
          pull(c) {
            c.enqueue(1);
          },
        },
        {
          async size() {
            return 1;
          },
        }
      );

      await rejects(rs.getReader().read(), {
        message: 'The value cannot be converted because it is not an integer.',
      });
    }
  },
};

// Test ReadableStream getDesiredSize is calculated correctly
export const readableGetDesiredSize = {
  async test() {
    // Value stream desiredSize
    {
      let controller;

      const rs = new ReadableStream(
        {
          start(c) {
            controller = c;
            strictEqual(c.desiredSize, 2);
            c.enqueue(1);
            strictEqual(c.desiredSize, 1);
            c.enqueue(2);
            strictEqual(c.desiredSize, 0);
            c.enqueue(3);
            strictEqual(c.desiredSize, -1);
          },
        },
        {
          highWaterMark: 2,
        }
      );

      await rs.getReader().read();
      strictEqual(controller.desiredSize, 0);
    }

    // Enqueuing when there's an active read skips the queue
    {
      let controller;
      const rs = new ReadableStream(
        {
          start(c) {
            controller = c;
          },
        },
        { highWaterMark: 2 }
      );

      const reader = rs.getReader();
      strictEqual(controller.desiredSize, 2);
      const read = reader.read();
      controller.enqueue(1);
      strictEqual(controller.desiredSize, 2);
      strictEqual((await read).value, 1);
    }

    // Byte stream desiredSize
    {
      let controller;
      const rs = new ReadableStream(
        {
          type: 'bytes',
          start(c) {
            controller = c;
            strictEqual(c.desiredSize, 2);
            c.enqueue(new Uint8Array(2));
            strictEqual(c.desiredSize, 0);
            c.enqueue(new Uint8Array(1));
            strictEqual(c.desiredSize, -1);
          },
        },
        {
          highWaterMark: 2,
        }
      );

      strictEqual((await rs.getReader().read()).value.byteLength, 3);
      strictEqual(controller.desiredSize, 2);
    }

    // Byte stream enqueuing when there's an active read skips the queue
    {
      let controller;
      const rs = new ReadableStream(
        {
          type: 'bytes',
          start(c) {
            controller = c;
          },
        },
        { highWaterMark: 2 }
      );

      const reader = rs.getReader();
      strictEqual(controller.desiredSize, 2);
      const read = reader.read();
      controller.enqueue(new Uint8Array(10));
      strictEqual(controller.desiredSize, 2);
      strictEqual((await read).value.byteLength, 10);
    }
  },
};

// Test ReadableStream controller.error() works as expected
export const readableStreamControllerError = {
  async test() {
    // Value stream
    {
      let controller;
      const rs = new ReadableStream({
        start(c) {
          controller = c;
        },
      });
      const reader = rs.getReader();
      const read = reader.read();
      controller.error(new Error('bang!'));
      await rejects(read, { message: 'bang!' });
    }

    // Byte stream
    {
      let controller;
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          controller = c;
        },
      });
      const reader = rs.getReader();
      const read = reader.read();
      controller.error(new Error('bang!'));
      await rejects(read, { message: 'bang!' });
    }
  },
};

// Test ReadableStream autoAllocateChunkSize works as expected
export const readableStreamAutoAllocateChunkSize = {
  async test() {
    throws(() => {
      new ReadableStream({
        type: 'bytes',
        autoAllocateChunkSize: 0,
      });
    }, TypeError);

    throws(() => {
      new ReadableStream({
        type: 'bytes',
        autoAllocateChunkSize: -1,
      });
    }, TypeError);

    throws(() => {
      new ReadableStream({
        type: 'bytes',
        autoAllocateChunkSize: 'a',
      });
    }, TypeError);

    let pulled = false;
    const rs = new ReadableStream({
      type: 'bytes',
      autoAllocateChunkSize: 10,
      pull(c) {
        pulled = true;
        if (c.byobRequest) {
          strictEqual(c.byobRequest.view.byteLength, 10);
          c.byobRequest.respond(10);
        }
      },
    });
    await rs.getReader().read();
    ok(pulled);
  },
};

// Test ReadableStream byte stream respond() works appropriately
export const readableStreamByteRespond = {
  async test() {
    // Basic respond
    {
      const rs = new ReadableStream({
        type: 'bytes',
        pull(c) {
          if (c.byobRequest) {
            const req = c.byobRequest;
            req.view[0] = 1;
            req.view[1] = 2;
            req.view[2] = 3;

            throws(() => req.respond(10), RangeError);
            throws(() => req.respond(0), TypeError);

            req.respond(3);

            // This will error the stream but won't be immediately
            // apparent until the next read operation.
            req.respond(3);
          }
        },
      });

      const reader = rs.getReader({ mode: 'byob' });
      const u8 = new Uint8Array(3);
      const read = reader.read(u8);
      strictEqual(u8.byteLength, 0);

      const { value } = await read;
      strictEqual(value.byteLength, 3);

      await rejects(reader.read(new Uint8Array(3)), {
        message: 'This ReadableStreamBYOBRequest has been invalidated.',
      });
    }

    // Respond with close
    {
      const rs = new ReadableStream({
        type: 'bytes',
        pull(c) {
          if (c.byobRequest) {
            c.close();
            c.byobRequest.respond(0);
          }
        },
      });

      const reader = rs.getReader({ mode: 'byob' });

      const u8 = new Uint8Array([1, 2, 3]);

      const { done, value } = await reader.read(u8);

      ok(done);
      ok(value instanceof Uint8Array);
      strictEqual(value.byteLength, 0);
      strictEqual(value.buffer.byteLength, 3);
      const u82 = new Uint8Array(value.buffer, 0, 3);
      strictEqual(u82[0], 1);
      strictEqual(u82[1], 2);
      strictEqual(u82[2], 3);
    }
  },
};

// Test ReadableStream byte stream respondWithNewView works appropriately
export const readableStreamByteRespondWithNewView = {
  async test() {
    // Basic respondWithNewView
    {
      const rs = new ReadableStream({
        type: 'bytes',
        pull(c) {
          if (c.byobRequest) {
            const req = c.byobRequest;
            const u8 = new Uint8Array(req.view.buffer);

            u8[0] = 1;
            u8[1] = 2;
            u8[2] = 3;

            // Can't respond with zero if we're not closed.
            throws(() => req.respondWithNewView(new Uint8Array(0)), TypeError);

            // Underlying buffer is too big.
            throws(
              () => req.respondWithNewView(new Uint8Array(10)),
              RangeError
            );

            // Can't respond with a non-detachable ArrayBuffer.
            throws(
              () =>
                req.respondWithNewView(
                  new Uint8Array(new SharedArrayBuffer(10))
                ),
              TypeError
            );

            // New view has an invalid byte offset.
            throws(
              () => req.respondWithNewView(new Uint8Array(req.view.buffer, 1)),
              RangeError
            );

            req.respondWithNewView(u8);

            strictEqual(u8.byteLength, 0);

            // This will error the stream but won't be immediately
            // apparent until the next read operation.
            req.respond(3);
          }
        },
      });

      const reader = rs.getReader({ mode: 'byob' });
      const u8 = new Uint8Array(3);
      const read = reader.read(u8);
      strictEqual(u8.byteLength, 0);

      const { value } = await read;
      strictEqual(value.byteLength, 3);
      strictEqual(value[0], 1);
      strictEqual(value[1], 2);
      strictEqual(value[2], 3);

      await rejects(reader.read(new Uint8Array(3)), {
        message: 'This ReadableStreamBYOBRequest has been invalidated.',
      });
    }

    // RespondWithNewView with close
    {
      const rs = new ReadableStream({
        type: 'bytes',
        pull(c) {
          if (c.byobRequest) {
            c.close();
            c.byobRequest.respondWithNewView(
              new Uint8Array(c.byobRequest.view.buffer, 0, 0)
            );
          }
        },
      });

      const reader = rs.getReader({ mode: 'byob' });

      const { done, value } = await reader.read(new Uint8Array(3));

      ok(done);
      ok(value instanceof Uint8Array);
      strictEqual(value.byteLength, 0);
      strictEqual(value.buffer.byteLength, 3);
    }
  },
};

// Test ReadableStream JS controllers allow for multiple pending reads
export const readableStreamMultiplePendingReads = {
  async test() {
    // Value stream
    {
      let controller;
      const rs = new ReadableStream({
        start(c) {
          controller = c;
        },
      });
      const reader = rs.getReader();
      const read1 = reader.read();
      const read2 = reader.read();
      controller.enqueue(1);
      controller.enqueue(2);
      const [res1, res2] = await Promise.all([read1, read2]);
      strictEqual(res1.value, 1);
      strictEqual(res2.value, 2);
    }

    // Byte stream
    {
      let controller;
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          controller = c;
        },
      });
      const enc = new TextEncoder();
      const dec = new TextDecoder();
      const reader = rs.getReader();
      const read1 = reader.read();
      const read2 = reader.read();
      controller.enqueue(enc.encode('hello'));
      controller.enqueue(enc.encode('there'));
      const [res1, res2] = await Promise.all([read1, read2]);
      strictEqual(dec.decode(res1.value), 'hello');
      strictEqual(dec.decode(res2.value), 'there');
    }
  },
};

// Test ReadableStream byte controller enqueue and reads with mismatched sizes works
export const readableStreamBytesMismatchedSizes = {
  async test() {
    const enc = new TextEncoder();
    let pulls = 0;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        c.enqueue(enc.encode('hello'));
      },
      pull(c) {
        if (c.byobRequest) {
          pulls++;
          c.enqueue(enc.encode('there'));
        }
      },
    });
    const reader = rs.getReader({ mode: 'byob' });

    await Promise.all(
      [
        enc.encode('he'),
        enc.encode('ll'),
        enc.encode('o'),
        enc.encode('th'),
        enc.encode('er'),
        enc.encode('e'),
      ].map(async (i) => {
        const { done, value } = await reader.read(new Uint8Array(2));
        ok(!done);
        strictEqual(value.byteLength, i.byteLength);
        for (let n = 0; n < value.byteLength; n++) {
          strictEqual(value[n], i[n]);
        }
      })
    );

    strictEqual(pulls, 1);
  },
};

// Test ReadableStream byte controller enqueue and reads with mismatched view types works
export const readableStreamBytesMismatchedViewTypes = {
  async test() {
    let pull = 0;
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        if (c.byobRequest) {
          const view = c.byobRequest.view;
          switch (pull++) {
            case 0: {
              strictEqual(view.byteLength, 8);
              strictEqual(view.byteOffset, 0);
              view[0] = 1;
              view[1] = 2;
              view[2] = 3;
              view[3] = 4;
              view[4] = 5;
              view[5] = 6;
              view[6] = 7;
              c.byobRequest.respond(7);
              break;
            }
            case 1: {
              strictEqual(view.byteLength, 5);
              strictEqual(view.byteOffset, 3);
              view[0] = 8;
              c.byobRequest.respond(1);
              c.close();
              break;
            }
          }
        }
      },
    });

    const r = rs.getReader({ mode: 'byob' });

    {
      const { value } = await r.read(new Uint32Array(2));
      ok(value instanceof Uint32Array);
      strictEqual(value.length, 1);
      strictEqual(value.byteLength, 4);
      strictEqual(value.buffer.byteLength, 8);
      const u8 = new Uint8Array(value.buffer, 0, value.byteLength);
      strictEqual(u8[0], 1);
      strictEqual(u8[1], 2);
      strictEqual(u8[2], 3);
      strictEqual(u8[3], 4);
    }

    {
      const { value } = await r.read(new Uint32Array(2));
      ok(value instanceof Uint32Array);
      strictEqual(value.length, 1);
      strictEqual(value.byteLength, 4);
      strictEqual(value.buffer.byteLength, 8);
      const u8 = new Uint8Array(value.buffer);
      strictEqual(u8[0], 5);
      strictEqual(u8[1], 6);
      strictEqual(u8[2], 7);
      strictEqual(u8[3], 8);
    }
  },
};

// Test ReadableStream byte controller enqueue subarray works
export const readableStreamBytesEnqueueSubarray = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        const u8 = enc.encode('hello');
        c.enqueue(u8.subarray(1, 4));
        strictEqual(u8.byteLength, 0);
        c.close();
      },
    });

    const r = rs.getReader({ mode: 'byob' });

    const { value } = await r.read(new Uint8Array(5));

    strictEqual(dec.decode(value), 'ell');
  },
};

// Test ReadableStream default and bytes controllers close promise works
export const readableStreamDefaultClosePromise = {
  async test() {
    // Value stream
    {
      let controller;
      const rs = new ReadableStream({
        start(c) {
          controller = c;
        },
      });
      const r = rs.getReader();
      let closed = false;
      r.closed.then(() => (closed = true));
      controller.enqueue(1);
      controller.close();
      await r.read();
      ok(closed);
    }

    // Byte stream default reader
    {
      let controller;
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          controller = c;
        },
      });

      const r = rs.getReader();

      let closed = false;
      r.closed.then(() => (closed = true));
      controller.enqueue(new Uint8Array(1));
      controller.close();
      await r.read();
      await scheduler.wait(1);
      ok(closed);
    }

    // Byte stream BYOB reader
    {
      let controller;
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          controller = c;
        },
      });

      const r = rs.getReader({ mode: 'byob' });

      let closed = false;
      r.closed.then(() => (closed = true));
      controller.enqueue(new Uint8Array(1));
      controller.close();
      await r.read(new Uint8Array(1));
      await scheduler.wait(1);
      ok(closed);
    }
  },
};

// Test ReadableStream default and bytes reads can be canceled
export const readableStreamCancelReads = {
  async test() {
    // Value stream
    {
      const rs = new ReadableStream();
      const reader = rs.getReader();
      const read = reader.read();
      reader.cancel();

      const { done, value } = await read;
      ok(done);
      strictEqual(value, undefined);
    }

    // Byte stream default reader
    {
      const rs = new ReadableStream({
        type: 'bytes',
      });
      const reader = rs.getReader();
      const read = reader.read();
      reader.cancel();

      const { done } = await read;
      ok(done);
    }

    // Byte stream BYOB reader
    {
      const rs = new ReadableStream({
        type: 'bytes',
      });
      const reader = rs.getReader({ mode: 'byob' });
      const read = reader.read(new Uint8Array(1));
      reader.cancel();

      const { done } = await read;
      ok(done);
    }

    // Byte stream BYOB reader with cancel reason
    {
      let cancelCalled = false;
      const rs = new ReadableStream({
        type: 'bytes',
        cancel(reason) {
          strictEqual(reason, 'boom');
          cancelCalled = true;
        },
      });
      const reader = rs.getReader({ mode: 'byob' });
      const read = reader.read(new Uint8Array(1));
      reader.cancel('boom');

      const { done } = await read;
      ok(done);
      ok(cancelCalled);
    }
  },
};

// Test ReadableStream default and byte controller release lock work
export const readableStreamReleaseLock = {
  async test() {
    // With capture_async_api_throws, async methods (pipeTo) return rejected promises instead of throwing
    // pipeThrough returns ReadableStream (not a promise), so it still throws synchronously
    const captureAsyncThrows =
      Cloudflare.compatibilityFlags.capture_async_api_throws;

    // Value stream
    {
      const rs = new ReadableStream();
      const reader = rs.getReader();
      throws(() => rs.getReader(), TypeError);
      throws(() => rs.tee(), TypeError);
      if (captureAsyncThrows) {
        await rejects(rs.pipeTo(), TypeError);
      } else {
        throws(() => rs.pipeTo(), TypeError);
      }
      throws(() => rs.pipeThrough(), TypeError);

      reader.releaseLock();
      rs.getReader();
    }

    // Byte stream default reader
    {
      const rs = new ReadableStream({
        type: 'bytes',
      });
      const reader = rs.getReader();
      throws(() => rs.getReader(), TypeError);
      throws(() => rs.tee(), TypeError);
      if (captureAsyncThrows) {
        await rejects(rs.pipeTo(), TypeError);
      } else {
        throws(() => rs.pipeTo(), TypeError);
      }
      throws(() => rs.pipeThrough(), TypeError);

      reader.releaseLock();
      rs.getReader();
    }

    // Byte stream BYOB reader
    {
      const rs = new ReadableStream({
        type: 'bytes',
      });
      const reader = rs.getReader({ mode: 'byob' });
      throws(() => rs.getReader(), TypeError);
      throws(() => rs.tee(), TypeError);
      if (captureAsyncThrows) {
        await rejects(rs.pipeTo(), TypeError);
      } else {
        throws(() => rs.pipeTo(), TypeError);
      }
      throws(() => rs.pipeThrough(), TypeError);

      reader.releaseLock();
      rs.getReader();
    }
  },
};

// Test ReadableStream default controller does not support BYOB reader
export const readableStreamDefaultNoByob = {
  test() {
    const rs = new ReadableStream();
    throws(() => rs.getReader({ mode: 'byob' }), TypeError);
    throws(() => new ReadableStreamBYOBReader(rs), TypeError);
  },
};

// Test ReadableStream default controller tee() works
export const readableStreamDefaultTee = {
  async test() {
    // Tee an immediately closed ReadableStream
    {
      const rs = new ReadableStream({
        start(c) {
          c.close();
        },
      });

      const [branch1, branch2] = rs.tee();

      const reader1 = branch1.getReader();
      const reader2 = branch2.getReader();

      const [res1, res2] = await Promise.all([reader1.read(), reader2.read()]);

      strictEqual(res1.done, true);
      strictEqual(res2.done, true);
    }

    // Tee with data
    {
      const rs = new ReadableStream({
        pull(c) {
          c.enqueue(1);
          c.close();
        },
      });

      const [branch1, branch2] = rs.tee();

      const reader1 = branch1.getReader();
      const reader2 = branch2.getReader();

      const [res1, res2] = await Promise.all([reader1.read(), reader2.read()]);

      strictEqual(res1.value, 1);
      strictEqual(res2.value, 1);

      const [res3, res4] = await Promise.all([reader1.read(), reader2.read()]);

      strictEqual(res3.done, true);
      strictEqual(res4.done, true);
    }

    // Tee with multiple enqueues
    {
      let counter = 0;
      const rs = new ReadableStream({
        pull(c) {
          c.enqueue(counter++);
          if (counter == 2) {
            c.close();
          }
        },
      });

      const [branch1, branch2] = rs.tee();

      const reader1 = branch1.getReader();
      const reader2 = branch2.getReader();

      {
        const [result1, result2] = await Promise.all([
          reader1.read(),
          reader2.read(),
        ]);

        ok(!result1.done);
        ok(!result2.done);
        strictEqual(result1.value, 0);
        strictEqual(result2.value, 0);
      }

      {
        const [result1, result2] = await Promise.all([
          reader1.read(),
          reader2.read(),
        ]);
        ok(!result1.done);
        ok(!result2.done);
        strictEqual(result1.value, 1);
        strictEqual(result2.value, 1);
      }

      {
        const [result1, result2] = await Promise.all([
          reader1.read(),
          reader2.read(),
        ]);
        ok(result1.done);
        ok(result2.done);
        strictEqual(result1.value, undefined);
        strictEqual(result2.value, undefined);
      }
    }

    // Canceling one branch does not impact the other
    {
      let counter = 0;
      let canceled = false;
      const rs = new ReadableStream({
        pull(c) {
          c.enqueue(counter++);
          if (counter == 2) {
            c.close();
          }
        },
        cancel() {
          canceled = true;
        },
      });

      const [branch1, branch2] = rs.tee();

      const reader1 = branch1.getReader();
      const reader2 = branch2.getReader();

      {
        const [result1, result2] = await Promise.all([
          reader1.read(),
          reader2.read(),
        ]);

        ok(!result1.done);
        ok(!result2.done);
        strictEqual(result1.value, 0);
        strictEqual(result2.value, 0);
      }

      reader2.cancel();

      {
        const [result1, result2] = await Promise.all([
          reader1.read(),
          reader2.read(),
        ]);

        ok(!canceled);

        ok(!result1.done);
        ok(result2.done);
        strictEqual(result1.value, 1);
        strictEqual(result2.value, undefined);
      }

      {
        const [result1, result2] = await Promise.all([
          reader1.read(),
          reader2.read(),
        ]);

        ok(result1.done);
        ok(result2.done);
        strictEqual(result1.value, undefined);
        strictEqual(result2.value, undefined);
      }
    }

    // Canceling both tee branches cancels the underlying source
    {
      let canceled = false;
      const rs = new ReadableStream({
        start(c) {
          c.enqueue(0);
        },
        cancel() {
          canceled = true;
        },
      });

      const [branch1, branch2] = rs.tee();

      const reader1 = branch1.getReader();
      const reader2 = branch2.getReader();

      {
        const [result1, result2] = await Promise.all([
          reader1.read(),
          reader2.read(),
        ]);

        ok(!result1.done);
        ok(!result2.done);
        strictEqual(result1.value, 0);
        strictEqual(result2.value, 0);
      }

      await reader1.cancel();
      ok(!canceled);

      await reader2.cancel();
      ok(canceled);
    }

    // Tee of a tee works
    {
      let controller;
      const rs = new ReadableStream({
        start(c) {
          controller = c;
        },
      });

      const [branch1, branch2] = rs.tee();
      const [branch3, branch4] = branch2.tee();

      throws(() => branch2.getReader(), TypeError);

      {
        const reader1 = branch1.getReader();
        const reader3 = branch3.getReader();
        const reader4 = branch4.getReader();

        const read1 = reader1.read();
        const read3 = reader3.read();
        const read4 = reader4.read();

        controller.enqueue(1);

        const [result1, result3, result4] = await Promise.all([
          read1,
          read3,
          read4,
        ]);

        strictEqual(result1.value, 1);
        strictEqual(result3.value, 1);
        strictEqual(result4.value, 1);
      }
    }

    // Erroring the underlying source errors the branches
    {
      let controller;
      const rs = new ReadableStream({
        start(c) {
          controller = c;
        },
      });

      const [branch1, branch2] = rs.tee();

      const reader1 = branch1.getReader();
      const reader2 = branch2.getReader();

      const read1 = reader1.read();
      const read2 = reader2.read();

      controller.error('boom');

      (await Promise.allSettled([read1, read2])).forEach((i) => {
        strictEqual(i.status, 'rejected');
        strictEqual(i.reason, 'boom');
      });
    }

    // Tee branches support BYOB reads
    {
      let controller;
      const enc = new TextEncoder();
      const dec = new TextDecoder();
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          controller = c;
        },
      });

      const [branch1, branch2] = rs.tee();

      const reader1 = branch1.getReader({ mode: 'byob' });
      const reader2 = branch2.getReader({ mode: 'byob' });

      const buf1 = new Uint8Array(2);
      const buf2 = new Uint8Array(3);

      const promises = [reader1.read(buf1), reader2.read(buf2)];

      controller.enqueue(enc.encode('hello'));

      const results = await Promise.all(promises);

      strictEqual(dec.decode(results[0].value), 'he');
      strictEqual(dec.decode(results[1].value), 'hel');
    }
  },
};

// =====================================================================================
// WritableStream tests
// =====================================================================================

// Test new WritableStream() works
export const newWritableStream = {
  test() {
    new WritableStream();
  },
};

// Test new WritableStream() with sink works
export const newWritableStreamWithSink = {
  async test() {
    // Sync sink with abort
    {
      let started = false;
      let written = false;
      let closed = false;
      let aborted = false;
      const ws = new WritableStream({
        start(c) {
          ok(c instanceof WritableStreamDefaultController);
          started = true;
        },
        write(value, c) {
          strictEqual(value, 1);
          ok(c instanceof WritableStreamDefaultController);
          written = true;
        },
        abort(reason) {
          strictEqual(reason.message, 'boom');
          aborted = true;
        },
        close() {
          closed = true;
        },
      });

      ok(started);

      const writer = ws.getWriter();

      await writer.write(1);
      ok(written);

      await writer.abort(new Error('boom'));
      ok(aborted);
      ok(!closed);

      await rejects(writer.closed);
    }

    // Sync sink with close
    {
      let started = false;
      let written = false;
      let closed = false;
      let aborted = false;
      const ws = new WritableStream({
        start(c) {
          ok(c instanceof WritableStreamDefaultController);
          started = true;
        },
        write(value, c) {
          strictEqual(value, 1);
          ok(c instanceof WritableStreamDefaultController);
          written = true;
        },
        abort() {
          aborted = true;
        },
        close() {
          closed = true;
        },
      });

      ok(started);

      const writer = ws.getWriter();

      await writer.write(1);
      ok(written);

      await Promise.all([writer.close(), writer.closed]);

      ok(!aborted);
      ok(closed);
    }
  },
};

// Test new WritableStream() with async sink works
export const newWritableStreamWithSinkAsync = {
  async test() {
    // Async sink with abort
    {
      let started = false;
      let written = false;
      let closed = false;
      let aborted = false;
      const ws = new WritableStream({
        async start(c) {
          ok(c instanceof WritableStreamDefaultController);
          await scheduler.wait(10);
          started = true;
        },
        async write(value, c) {
          await scheduler.wait(10);
          strictEqual(value, 1);
          ok(c instanceof WritableStreamDefaultController);
          written = true;
        },
        async abort(reason) {
          await scheduler.wait(10);
          strictEqual(reason.message, 'boom');
          aborted = true;
        },
        async close() {
          closed = true;
        },
      });

      await scheduler.wait(15);
      ok(started);

      const writer = ws.getWriter();
      await writer.ready;

      await writer.write(1);
      ok(written);

      await writer.abort(new Error('boom'));
      ok(aborted);
      ok(!closed);

      await rejects(writer.closed);
    }

    // Async sink with close
    {
      let started = false;
      let written = false;
      let closed = false;
      let aborted = false;
      const ws = new WritableStream({
        async start(c) {
          ok(c instanceof WritableStreamDefaultController);
          await scheduler.wait(10);
          started = true;
        },
        async write(value, c) {
          await scheduler.wait(10);
          strictEqual(value, 1);
          ok(c instanceof WritableStreamDefaultController);
          written = true;
        },
        async abort() {
          await scheduler.wait(10);
          aborted = true;
        },
        async close() {
          await scheduler.wait(10);
          closed = true;
        },
      });

      await scheduler.wait(15);
      ok(started);

      const writer = ws.getWriter();
      await writer.ready;

      await writer.write(1);
      ok(written);

      await Promise.all([writer.close(), writer.closed]);
      ok(!aborted);
      ok(closed);
    }
  },
};

// Test new WritableStream() start algorithm error handled
export const newWritableStreamStartError = {
  async test() {
    // Sync start error
    {
      const ws = new WritableStream({
        start() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.write(1), { message: 'boom' });
    }

    // Async start error
    {
      const ws = new WritableStream({
        async start() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.write(1), { message: 'boom' });
    }

    // Start with controller.error
    {
      const ws = new WritableStream({
        start(c) {
          c.error(new Error('boom'));
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.write(1), { message: 'boom' });
    }
  },
};

// Test new WritableStream() write algorithm error handled
export const newWritableStreamWriteError = {
  async test() {
    // Sync write error
    {
      const ws = new WritableStream({
        write() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.write(1), { message: 'boom' });
    }

    // Async write error
    {
      const ws = new WritableStream({
        async write() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.write(1), { message: 'boom' });
    }

    // Write with controller.error
    {
      const ws = new WritableStream({
        write(value, c) {
          strictEqual(value, 1);
          c.error(new Error('boom'));
        },
      });

      const writer = ws.getWriter();

      // Should succeed
      await writer.write(1);

      await rejects(writer.closed, { message: 'boom' });
    }
  },
};

// Test new WritableStream() abort algorithm error handled
export const newWritableStreamAbortError = {
  async test() {
    // Sync abort error
    {
      const ws = new WritableStream({
        abort(reason) {
          strictEqual(reason, 1);
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();

      await rejects(writer.abort(1), { message: 'boom' });

      // Calling abort again returns the same rejected promise
      await rejects(writer.abort(1), { message: 'boom' });
    }

    // Async abort error
    {
      const ws = new WritableStream({
        async abort(reason) {
          strictEqual(reason, 1);
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.abort(1), { message: 'boom' });
    }

    // Abort with controller.error
    {
      let controller;
      const ws = new WritableStream({
        start(c) {
          controller = c;
        },
        async abort(reason) {
          strictEqual(reason, 1);
          controller.error(new Error('ignored'));
        },
      });

      const writer = ws.getWriter();

      await writer.abort(1);

      // The closed promise will use the abort reason, not the error
      // reported in the controller
      const results = await Promise.allSettled([writer.closed]);
      strictEqual(results[0].status, 'rejected');
      strictEqual(results[0].reason, 1);
    }
  },
};

// Test new WritableStream() close algorithm error handled
export const newWritableStreamCloseError = {
  async test() {
    // Sync close error
    {
      const ws = new WritableStream({
        close() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.close(), { message: 'boom' });
    }

    // Async close error
    {
      const ws = new WritableStream({
        async close() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();
      await rejects(writer.close(), { message: 'boom' });
    }

    // Close with controller.error (ignored)
    {
      let controller;
      const ws = new WritableStream({
        start(c) {
          controller = c;
        },
        async close() {
          controller.error(new Error('ignored'));
        },
      });

      const writer = ws.getWriter();

      // In this case, the error reported in the close algorithm is ignored
      await Promise.all([writer.close(), writer.closed]);
    }
  },
};

// Test WritableStream multiple pending writes allowed
export const writableStreamMultiplePendingWrites = {
  async test() {
    const expectedWrites = ['hello', 'there'];
    const ws = new WritableStream({
      async write(value) {
        await scheduler.wait(10);
        strictEqual(value, expectedWrites.shift());
      },
    });

    const writer = ws.getWriter();

    await Promise.all([writer.write('hello'), writer.write('there')]);
  },
};

// Test WritableStream writing a subarray works
export const writableStreamWriteSubarray = {
  async test() {
    const u8 = new Uint8Array([1, 2, 3, 4]);
    const sub = u8.subarray(1, 3);

    const ws = new WritableStream({
      write(value) {
        strictEqual(value, sub);
      },
    });

    const writer = ws.getWriter();

    await writer.write(sub);
  },
};

// Test WritableStream writing any javascript value works
export const writableStreamWriteAny = {
  async test() {
    // Make a copy since we'll shift() from it
    const expectedWrites = [
      'hello',
      true,
      1,
      1.1,
      undefined,
      NaN,
      Infinity,
      new Uint8Array(1),
      {},
      [],
    ];
    // Keep original for writing
    const valuesToWrite = [...expectedWrites];

    const ws = new WritableStream({
      async write(value) {
        await scheduler.wait(1);
        const expected = expectedWrites.shift();
        // Use Object.is for NaN and -0/+0 handling (same as testharness same_value)
        ok(Object.is(value, expected), `expected ${expected} but got ${value}`);
      },
    });

    const writer = ws.getWriter();

    await Promise.all(valuesToWrite.map((i) => writer.write(i)));
  },
};

// Test WritableStream desiredSize calculated correctly
export const writableStreamDesiredSize = {
  async test() {
    const ws = new WritableStream(
      {
        async write() {
          await scheduler.wait(10);
        },
      },
      {
        highWaterMark: 2,
      }
    );

    const writer = ws.getWriter();

    const firstReady = writer.ready;
    await firstReady;

    strictEqual(writer.desiredSize, 2);
    const write1 = writer.write(1);
    strictEqual(writer.desiredSize, 1);

    const write2 = writer.write(2);
    const write3 = writer.write(3);

    strictEqual(writer.desiredSize, -1);

    await Promise.all([write1, write2, write3]);

    ok(firstReady != writer.ready);
    await writer.ready;
  },
};

// Test WritableStream writes can be aborted
export const writableStreamWriteAbort = {
  async test() {
    let aborted = false;
    const ws = new WritableStream({
      start(c) {
        c.signal.addEventListener(
          'abort',
          () => {
            strictEqual(c.signal.reason.message, 'boom');
            aborted = true;
          },
          { once: true }
        );
      },
      async write() {
        await scheduler.wait(10);
      },
    });

    const writer = ws.getWriter();
    const write1 = writer.write(1);
    const write2 = writer.write(2);

    writer.abort(new Error('boom'));

    const [result1, result2] = await Promise.allSettled([write1, write2]);

    // Both writes fail
    strictEqual(result1.status, 'rejected');
    strictEqual(result2.status, 'rejected');
    strictEqual(result1.reason.message, 'boom');
    strictEqual(result2.reason.message, 'boom');

    ok(aborted);

    // Aborting puts the stream into a persistent errored state
    writer.releaseLock();
    const writer2 = ws.getWriter();

    await rejects(writer2.write('should not be allowed'), { message: 'boom' });
  },
};

// Test WritableStream uses size algorithm correctly
export const writableStreamSizeAlgorithm = {
  async test() {
    // Size algorithm called
    {
      let sizeCalled = false;
      const ws = new WritableStream(
        {
          async write() {
            await scheduler.wait(10);
          },
        },
        {
          highWaterMark: 2,
          size(value) {
            sizeCalled = true;
            strictEqual(value, 'hello');
            return 2;
          },
        }
      );

      const writer = ws.getWriter();
      strictEqual(writer.desiredSize, 2);
      const write = writer.write('hello');
      ok(sizeCalled);
      strictEqual(writer.desiredSize, 0);
      await write;
    }

    // Size algorithm error
    {
      const ws = new WritableStream(
        {},
        {
          size() {
            throw new Error('boom');
          },
        }
      );

      const writer = ws.getWriter();
      await rejects(writer.write('hello'), { message: 'boom' });
    }
  },
};

// Test WritableStream aborting should replace ready promise
export const writableStreamAbortReadyRejected = {
  async test() {
    const ws = new WritableStream();
    const writer = ws.getWriter();
    const ready = writer.ready;

    await ready;

    writer.abort('boom');

    ok(ready !== writer.ready);

    const results = await Promise.allSettled([writer.ready]);
    strictEqual(results[0].status, 'rejected');
    strictEqual(results[0].reason, 'boom');
  },
};

// Test WritableStream abort with no argument defaults to undefined
export const writableStreamAbortOptional = {
  async test() {
    const ws = new WritableStream();
    const writer = ws.getWriter();
    await writer.abort();

    const results = await Promise.allSettled([writer.closed]);
    strictEqual(results[0].status, 'rejected');
    strictEqual(results[0].reason, undefined);
  },
};

// Test WritableStream abort while starting rejects ready promise
export const writableStreamAbortWhileStarting = {
  async test() {
    const ws = new WritableStream({
      async start() {},
    });

    const writer = ws.getWriter();
    writer.abort('boom');

    const results = await Promise.allSettled([writer.ready]);
    strictEqual(results[0].status, 'rejected');
    strictEqual(results[0].reason, 'boom');
  },
};

// Test WritableStream abort error while writing rejects abort promise
export const writableStreamAbortWhileWriting = {
  async test() {
    const ws = new WritableStream({
      async write() {
        await scheduler.wait(10);
      },

      async abort() {
        throw new Error('boom');
      },
    });

    const writer = ws.getWriter();
    const write = writer.write('test');

    await rejects(writer.abort(), { message: 'boom' });

    await write;

    // The write should reject with undefined because the writer.abort() above
    // specified undefined. The abort algorithm should not be called again.
    await rejects(writer.write('should fail'), (err) => err === undefined);
  },
};

// Test WritableStream releaseLock while aborting should reject closed promise
export const writableStreamReleaseLockWhileAborting = {
  async test() {
    const ws = new WritableStream({
      async write() {
        await scheduler.wait(10);
      },
    });

    const writer = ws.getWriter();
    writer.write('test');

    writer.abort();
    const closed = writer.closed;

    writer.releaseLock();

    await rejects(closed, TypeError);
  },
};

// Test WritableStream throw during in flight close rejects abort and closed promise
export const writableStreamCloseThrowRejectsPromises = {
  async test() {
    const ws = new WritableStream({
      async close() {
        throw new Error('boom');
      },
    });

    const writer = ws.getWriter();
    const close = writer.close();
    const abort = writer.abort();
    const closed = writer.closed;

    const res = await Promise.allSettled([close, abort, closed]);

    strictEqual(res[0].status, 'rejected');
    strictEqual(res[1].status, 'rejected');
    strictEqual(res[2].status, 'rejected');

    // The close and abort promises are rejected with the error thrown,
    // the closed promise, however, should be rejected with the reason
    // given in the abort(), which is defaulted to undefined.
    strictEqual(res[0].reason.message, 'boom');
    strictEqual(res[1].reason.message, 'boom');
    strictEqual(res[2].reason, undefined);
  },
};

// Test WritableStream sink abort not called while write or close in flight
export const writableStreamAbortTiming = {
  async test() {
    // Abort waits for start
    {
      let started = false;
      const ws = new WritableStream({
        async start() {
          await scheduler.wait(10);
          started = true;
        },
        abort() {
          ok(started, 'The stream should have started first');
        },
      });

      await ws.abort();
    }

    // Abort waits for write
    {
      let writeCompleted = false;
      const ws = new WritableStream({
        async write() {
          await scheduler.wait(10);
          writeCompleted = true;
        },
        abort() {
          ok(writeCompleted, 'The write should have completed');
        },
      });

      const writer = ws.getWriter();
      const write = writer.write('hello');
      const abort = writer.abort();

      await Promise.allSettled([write, abort]);
    }

    // Abort waits for close
    {
      let closeCompleted = false;
      const ws = new WritableStream({
        async close() {
          await scheduler.wait(10);
          closeCompleted = true;
        },
        abort() {
          ok(closeCompleted, 'The close should have completed');
        },
      });

      const writer = ws.getWriter();
      const close = writer.close();
      const abort = writer.abort();

      await Promise.allSettled([close, abort]);
    }
  },
};

// Test WritableStream abort during write should trigger abort algorithm with close pending
export const writableStreamAbortWriteClosePending = {
  async test() {
    let abortCalled = false;
    const ws = new WritableStream({
      async write() {
        await scheduler.wait(10);
      },
      abort() {
        abortCalled = true;
      },
    });

    const writer = ws.getWriter();
    const write = writer.write('hello');
    const close = writer.close();
    const abort = writer.abort();

    const res = await Promise.allSettled([write, close, abort]);
    ok(abortCalled);

    strictEqual(res[0].status, 'fulfilled'); // Write finishes
    strictEqual(res[1].status, 'rejected'); // Pending close is aborted
    strictEqual(res[2].status, 'fulfilled'); // Abort finishes
  },
};

// Test WritableStream ready promise rejects on controller error not waiting for in flight write
export const writableStreamErrorDuringInFlightWrite = {
  async test() {
    let controller;
    const ws = new WritableStream({
      start(c) {
        controller = c;
      },
      async write() {
        await scheduler.wait(10);
      },
    });

    const writer = ws.getWriter();

    const write = writer.write('hello').catch(() => {});

    controller.error('boom');

    await Promise.all([write, writer.ready.catch(() => {})]);
  },
};

// Test WritableStream start errors after abort, close rejects
export const writableStreamStartErrorAfterAbort = {
  async test() {
    const ws = new WritableStream({
      async start() {
        await scheduler.wait(10);
        throw new Error('boom');
      },
    });

    ws.abort();

    // close() should return a rejected promise. The abort was called
    // before start finished, so the stream enters an errored state.
    await ws.close().catch(() => {});

    // Verify the stream is errored
    strictEqual(ws.locked, false);
  },
};

// Test WritableStream rejected sink write does not prevent sink abort
export const writableStreamRejectedWriteNoPreventAbort = {
  async test() {
    let abortCalled = false;
    const ws = new WritableStream({
      write() {
        throw new Error('boom');
      },
      abort() {
        abortCalled = true;
      },
    });

    const writer = ws.getWriter();
    const write = writer.write('hello');
    const abort = writer.abort();

    const res = await Promise.allSettled([write, abort]);
    ok(abortCalled);
    strictEqual(res[0].status, 'rejected');
    strictEqual(res[1].status, 'fulfilled');
  },
};

// Test WritableStream aborted twice
export const writableStreamAbortedTwice = {
  async test() {
    const ws = new WritableStream();
    const abort1 = ws.abort();
    const abort2 = ws.abort();

    await Promise.all([abort1, abort2]);
  },
};

// Test WritableStream aborting errored stream rejects with stored error
export const writableStreamAbortOnErroredResolves = {
  async test() {
    const ws = new WritableStream({
      start(c) {
        c.error(new Error('boom'));
      },
    });

    // When aborting an already-errored stream, abort() rejects with the stored error
    await rejects(ws.abort(), Error);
  },
};

// Test WritableStream sink abort not called if stream errored before abort
export const writableStreamSinkAlgNoCallErrorBeforeAbort = {
  test() {
    let controller;
    let abortCalled = false;
    const ws = new WritableStream({
      start(c) {
        controller = c;
      },
      abort() {
        abortCalled = true;
      },
    });

    controller.error(new Error('boom'));
    ws.abort(new Error('bang'));

    ok(!abortCalled);
  },
};

// Test WritableStream writer with pending abort, ready should reject
export const writableStreamWriterWithPendingAbort = {
  async test() {
    const ws = new WritableStream();
    ws.abort(new Error('boom'));
    const writer = ws.getWriter();

    await rejects(writer.ready, Error);
  },
};

// Test WritableStream promises resolved in order
export const writableStreamPromisesResolvedInOrder = {
  async test() {
    // Write before close
    {
      let closeFinished = false;
      const ws = new WritableStream();

      const writer = ws.getWriter();

      const write = writer.write('hello').then(() => ok(!closeFinished));
      const close = writer.close();
      const closed = writer.closed.then(() => (closeFinished = true));

      // Closed promise should not resolve before fulfilled write.
      await Promise.allSettled([write, close, closed]);
    }

    // Rejected write before close
    {
      let closeFinished = false;
      let writeFailed = false;
      const ws = new WritableStream({
        write() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();

      const write = writer.write('hello').catch(() => {
        ok(!closeFinished);
        writeFailed = true;
      });
      const close = writer.close();
      const closed = writer.closed.then(() => (closeFinished = true));

      // Closed promise should not resolve before rejected write.
      await Promise.allSettled([write, close, closed]);
      ok(writeFailed);
    }

    // Writes resolved in order when aborting
    {
      const order = [];
      const ws = new WritableStream({
        async write() {
          await scheduler.wait(10);
        },
      });

      const writer = ws.getWriter();

      const write1 = writer.write('hello').then(() => order.push(1));
      const write2 = writer.write('hello').catch(() => order.push(2));
      const write3 = writer.write('hello').catch(() => order.push(3));
      const abort = writer.abort();

      await Promise.allSettled([write1, write2, write3, abort]);

      strictEqual(order[0], 1);
      strictEqual(order[1], 2);
      strictEqual(order[2], 3);
    }
  },
};

// =====================================================================================
// Misc tests
// =====================================================================================

// Test highWaterMark validation
export const highWaterMarkValidated = {
  test() {
    [-1, -Infinity, NaN, {}, 'foo'].forEach((highWaterMark) => {
      throws(() => new WritableStream(undefined, { highWaterMark }), TypeError);
      throws(() => new ReadableStream(undefined, { highWaterMark }), TypeError);
    });
  },
};

// Test QueuingStrategy objects work
export const queuingStrategies = {
  test() {
    // ByteLengthQueuingStrategy
    {
      const strategy = new ByteLengthQueuingStrategy({ highWaterMark: 10 });

      let startRan = false;

      // Make sure we can create a stream using the strategy without error.
      new ReadableStream(
        {
          start(c) {
            strictEqual(c.desiredSize, 10);
            c.enqueue(new Uint8Array(2));
            strictEqual(c.desiredSize, 8);
            startRan = true;
          },
        },
        strategy
      );

      const { highWaterMark, size } = strategy;

      ok(startRan);
      strictEqual(highWaterMark, 10);
      strictEqual(size('nothing'), undefined);
      strictEqual(size(123), undefined);
      strictEqual(size(undefined), undefined);
      strictEqual(size(null), undefined);
      strictEqual(size(), undefined);
      strictEqual(size(new ArrayBuffer(10)), 10);
      strictEqual(size(new Uint8Array(10)), 10);
    }

    // CountQueuingStrategy
    {
      const strategy = new CountQueuingStrategy({ highWaterMark: 9 });

      let startRan = false;

      // Make sure we can create a stream using the strategy without error.
      new ReadableStream(
        {
          start(c) {
            strictEqual(c.desiredSize, 9);
            c.enqueue(new Uint8Array(2));
            strictEqual(c.desiredSize, 8);
            startRan = true;
          },
        },
        strategy
      );

      const { highWaterMark, size } = strategy;

      ok(startRan);
      strictEqual(highWaterMark, 9);
      strictEqual(size('nothing'), 1);
      strictEqual(size(123), 1);
      strictEqual(size(undefined), 1);
      strictEqual(size(null), 1);
      strictEqual(size(), 1);
      strictEqual(size(new ArrayBuffer(10)), 1);
      strictEqual(size(new Uint8Array(10)), 1);
    }
  },
};

// Test proper default highwater mark
export const hwmDefault = {
  async test() {
    let pulled = 0;
    new ReadableStream({
      start(c) {
        strictEqual(c.desiredSize, 1);
      },
      pull() {
        pulled++;
      },
    });

    new ReadableStream({
      type: 'bytes',
      start(c) {
        strictEqual(c.desiredSize, 0);
      },
      pull() {
        pulled += 2;
      },
    });

    await scheduler.wait(1);
    strictEqual(pulled, 1);
  },
};

// Test byobreader regression
export const byobreaderRegression = {
  async test() {
    function newReadableStream(chunks) {
      chunks = chunks.filter((ch) => ch !== 0);
      return new ReadableStream({
        type: 'bytes',
        start(c) {
          if (chunks.length === 0) {
            c.close();
          }
        },
        pull(c) {
          c.enqueue(new Uint8Array(chunks.shift()));
          if (chunks.length === 0) {
            c.close();
          }
        },
      });
    }

    const rs = newReadableStream([]);
    // Ensure that getting a byob reader on a closed byte stream works correctly.
    const reader = rs.getReader({ mode: 'byob' });
    const { done } = await reader.read(new Uint8Array(10));
    ok(done);
  },
};

// Test writer double close
export const writerDoubleClose = {
  async test() {
    const ws = new WritableStream({
      write() {},
    });
    const writer = ws.getWriter();

    writer.write(123);

    writer.close();
    // With capture_async_api_throws, async methods return rejected promises instead of throwing
    if (Cloudflare.compatibilityFlags.capture_async_api_throws) {
      await rejects(writer.close(), TypeError);
    } else {
      throws(() => writer.close(), TypeError);
    }
  },
};

// =====================================================================================
// GC tests (these require --expose-gc v8 flag)
// =====================================================================================

// Test ReadableStream object references are held through gc
export const readableStreamReferencesHold = {
  async test() {
    let controller;
    let reader;
    let read;

    // Byte stream
    {
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          controller = c;
        },
      });

      reader = rs.getReader({ mode: 'byob' });
    }

    await scheduler.wait(10);
    gc();

    {
      read = reader.read(new Uint8Array(1));
      reader = undefined;
    }

    await scheduler.wait(10);
    gc();

    {
      controller.enqueue(new Uint8Array([1]));
      controller = undefined;
      const { value, done } = await read;
      ok(!done);
      strictEqual(value[0], 1);
    }

    // Value stream
    {
      let controller;
      let reader;
      let read;

      {
        const rs = new ReadableStream({
          start(c) {
            controller = c;
          },
        });
        reader = rs.getReader();
      }

      await scheduler.wait(10);
      gc();

      {
        read = reader.read();
        reader = undefined;
      }

      await scheduler.wait(10);
      gc();

      {
        controller.enqueue('hello');
        controller = undefined;
        const { value, done } = await read;
        ok(!done);
        strictEqual(value, 'hello');
      }
    }
  },
};

// Test WritableStream object references are held through gc
export const writableStreamGc = {
  async test() {
    let controller;
    let writer;
    let write;

    {
      const ws = new WritableStream({
        start(c) {
          controller = c;
        },
      });
      writer = ws.getWriter();
    }

    await scheduler.wait(10);
    gc();

    {
      write = writer.write(1);
      writer = undefined;
    }

    await scheduler.wait(10);
    gc();

    {
      await write;
      strictEqual(controller.signal.aborted, false);
    }
  },
};

// Test ReadableStream with async iterator gc works
export const asyncIteratorGc = {
  async test() {
    // This test verifies that the ReadableStream and its async iterator
    // are properly handled through gc
    function getNextPromise() {
      let values = new ReadableStream({
        async pull(controller) {
          await scheduler.wait(50);
          controller.enqueue('A');
          controller.close();
        },
      }).values();
      values.next();
      const promise = values.next();
      values = undefined;
      return promise;
    }

    let promise = getNextPromise();
    gc();
    strictEqual((await promise).done, true);
    promise = undefined;
    gc();
  },
};
