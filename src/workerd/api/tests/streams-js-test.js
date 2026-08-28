// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for JavaScript-backed streams (ReadableStream constructors).
// Ported from edgeworker streams-js.ew-test. The WritableStream tests
// live in the writable suite (src/tests/streams/writable/).

import { strictEqual, ok, throws, rejects } from 'node:assert';

// Test that JS streams globals exist
export const userStreamsGlobalsExist = {
  test() {
    ok(ReadableStreamDefaultController !== undefined);
    ok(ReadableByteStreamController !== undefined);
    ok(ReadableStreamBYOBRequest !== undefined);
  },
};

// Test that JS streams objects are not directly constructable
export const jsStreamsObjectsNotConstructable = {
  test() {
    throws(() => new ReadableStreamDefaultController(), TypeError);
    throws(() => new ReadableByteStreamController(), TypeError);
    throws(() => new ReadableStreamBYOBRequest(), TypeError);
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
      let thrown = false;
      const rs = new ReadableStream({
        pull() {
          if (!thrown) {
            thrown = true;
            throw new Error('boom');
          }
        },
      });

      const reader = rs.getReader();
      await rejects(reader.read(), { message: 'boom' });
      // Verify the stream is persistently errored, not just pull throwing again.
      await rejects(reader.read(), { message: 'boom' });
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
      let thrown = false;
      const rs = new ReadableStream({
        async pull() {
          if (!thrown) {
            thrown = true;
            throw new Error('boom');
          }
        },
      });

      const reader = rs.getReader();
      await rejects(reader.read(), { message: 'boom' });
      // Verify the stream is persistently errored, not just pull throwing again.
      await rejects(reader.read(), { message: 'boom' });
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

export const readableStreamByteRespondWithNewViewUsesNewElementSize = {
  async test() {
    const rs = new ReadableStream({
      type: 'bytes',
      pull(controller) {
        const request = controller.byobRequest;
        const replacement = new Uint16Array(
          request.view.buffer,
          request.view.byteOffset,
          3
        );

        new Uint8Array(
          replacement.buffer,
          replacement.byteOffset,
          replacement.byteLength
        ).set([1, 2, 3, 4, 5, 6]);

        request.respondWithNewView(replacement);
        controller.close();
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    const { value, done } = await reader.read(new Uint32Array(2));

    ok(!done);
    strictEqual(value.byteLength, 6);

    const bytes = new Uint8Array(
      value.buffer,
      value.byteOffset,
      value.byteLength
    );
    for (let i = 0; i < bytes.length; i++) {
      strictEqual(bytes[i], i + 1);
    }

    // Ensure no bytes were incorrectly shaved off and queued.
    const end = await reader.read(new Uint8Array(1));
    ok(end.done);
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
// Misc tests
// =====================================================================================

// Test highWaterMark validation
export const highWaterMarkValidated = {
  test() {
    [-1, -Infinity, NaN, {}, 'foo'].forEach((highWaterMark) => {
      throws(() => new ReadableStream(undefined, { highWaterMark }), TypeError);
    });
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
