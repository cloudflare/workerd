// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStreamBYOBReader basics: the view-type matrix (Uint16/32,
// Float32/64, DataView, offsets, mixed types), auto-allocate sizing, and
// partial-respond alignment. Migrated from
// streams-byob-edge-cases-test.js (all parity).

import { strictEqual, ok } from 'node:assert';

// Helper to create a byte stream that responds with data
function createByteStreamWithData(data) {
  return new ReadableStream({
    type: 'bytes',
    pull(controller) {
      if (controller.byobRequest) {
        const view = controller.byobRequest.view;
        const bytesToCopy = Math.min(view.byteLength, data.length);
        new Uint8Array(view.buffer, view.byteOffset, bytesToCopy).set(
          data.subarray(0, bytesToCopy)
        );
        data = data.subarray(bytesToCopy);
        controller.byobRequest.respond(bytesToCopy);
        if (data.length === 0) {
          controller.close();
        }
      } else {
        controller.enqueue(data);
        controller.close();
      }
    },
  });
}

// Test BYOB read with Uint16Array view
// Inspired by: Bun test/js/node/test/parallel/test-whatwg-readablebytestream.js
export const byobUint16Array = {
  async test() {
    // Create data that's properly aligned for Uint16Array (even number of bytes)
    const data = new Uint8Array([0x01, 0x02, 0x03, 0x04, 0x05, 0x06]);
    const rs = createByteStreamWithData(data);

    const reader = rs.getReader({ mode: 'byob' });

    // Read with Uint16Array (3 elements = 6 bytes)
    const view = new Uint16Array(3);
    const { value, done } = await reader.read(view);

    ok(!done);
    ok(value instanceof Uint16Array);
    strictEqual(value.length, 3);
    strictEqual(value.byteLength, 6);

    // Verify the data (endianness-dependent)
    const asBytes = new Uint8Array(
      value.buffer,
      value.byteOffset,
      value.byteLength
    );
    strictEqual(asBytes[0], 0x01);
    strictEqual(asBytes[1], 0x02);

    reader.releaseLock();
  },
};

// Test BYOB read with Uint32Array view
// Inspired by: Bun test/js/node/test/parallel/test-whatwg-readablebytestream.js
export const byobUint32Array = {
  async test() {
    // Create data aligned for Uint32Array (multiple of 4 bytes)
    const data = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
    const rs = createByteStreamWithData(data);

    const reader = rs.getReader({ mode: 'byob' });

    const view = new Uint32Array(2);
    const { value, done } = await reader.read(view);

    ok(!done);
    ok(value instanceof Uint32Array);
    strictEqual(value.length, 2);
    strictEqual(value.byteLength, 8);

    reader.releaseLock();
  },
};

// Test BYOB read with Float32Array view
// Inspired by: Bun test/js/node/test/parallel/test-whatwg-readablebytestream.js
export const byobFloat32Array = {
  async test() {
    // Create 8 bytes of data (2 Float32 values)
    const data = new Uint8Array(8);
    const floatView = new Float32Array(data.buffer);
    floatView[0] = 3.14;
    floatView[1] = 2.71;

    const rs = createByteStreamWithData(data);
    const reader = rs.getReader({ mode: 'byob' });

    const view = new Float32Array(2);
    const { value, done } = await reader.read(view);

    ok(!done);
    ok(value instanceof Float32Array);
    strictEqual(value.length, 2);
    ok(Math.abs(value[0] - 3.14) < 0.001);
    ok(Math.abs(value[1] - 2.71) < 0.001);

    reader.releaseLock();
  },
};

// Test BYOB read with Float64Array view
// Inspired by: Bun test/js/node/test/parallel/test-whatwg-readablebytestream.js
export const byobFloat64Array = {
  async test() {
    // Create 16 bytes of data (2 Float64 values)
    const data = new Uint8Array(16);
    const floatView = new Float64Array(data.buffer);
    floatView[0] = Math.PI;
    floatView[1] = Math.E;

    const rs = createByteStreamWithData(data);
    const reader = rs.getReader({ mode: 'byob' });

    const view = new Float64Array(2);
    const { value, done } = await reader.read(view);

    ok(!done);
    ok(value instanceof Float64Array);
    strictEqual(value.length, 2);
    ok(Math.abs(value[0] - Math.PI) < 0.0001);
    ok(Math.abs(value[1] - Math.E) < 0.0001);

    reader.releaseLock();
  },
};

// Test BYOB read with DataView
// Inspired by: Bun test/js/node/test/parallel/test-whatwg-readablebytestreambyob.js
export const byobDataView = {
  async test() {
    const data = new Uint8Array([0xde, 0xad, 0xbe, 0xef]);
    const rs = createByteStreamWithData(data);

    const reader = rs.getReader({ mode: 'byob' });

    const view = new DataView(new ArrayBuffer(4));
    const { value, done } = await reader.read(view);

    ok(!done);
    ok(value instanceof DataView);
    strictEqual(value.byteLength, 4);
    strictEqual(value.getUint8(0), 0xde);
    strictEqual(value.getUint8(1), 0xad);
    strictEqual(value.getUint8(2), 0xbe);
    strictEqual(value.getUint8(3), 0xef);

    reader.releaseLock();
  },
};

// Test using different view types across consecutive reads
// Inspired by: workerd streams-js-test.js readableStreamBytesMismatchedViewTypes
export const byobMixedViewTypes = {
  async test() {
    let pullCount = 0;
    const rs = new ReadableStream({
      type: 'bytes',
      pull(controller) {
        pullCount++;
        if (controller.byobRequest) {
          const view = controller.byobRequest.view;
          const u8 = new Uint8Array(
            view.buffer,
            view.byteOffset,
            view.byteLength
          );
          for (let i = 0; i < u8.length; i++) {
            u8[i] = pullCount * 10 + i;
          }
          controller.byobRequest.respond(view.byteLength);
        }
      },
    });

    const reader = rs.getReader({ mode: 'byob' });

    const result1 = await reader.read(new Uint8Array(4));
    ok(result1.value instanceof Uint8Array);
    strictEqual(result1.value.byteLength, 4);

    const result2 = await reader.read(new Uint16Array(2));
    ok(result2.value instanceof Uint16Array);
    strictEqual(result2.value.byteLength, 4);

    const result3 = await reader.read(new Uint32Array(1));
    ok(result3.value instanceof Uint32Array);
    strictEqual(result3.value.byteLength, 4);

    strictEqual(pullCount, 3);
    reader.releaseLock();
  },
};

// Test BYOB read with ArrayBufferView that has a non-zero byteOffset
// Inspired by: Bun test/js/node/test/parallel/test-whatwg-readablebytestream.js
export const byobViewOffset = {
  async test() {
    const data = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
    const rs = createByteStreamWithData(data);

    const reader = rs.getReader({ mode: 'byob' });

    const buffer = new ArrayBuffer(16);
    const view = new Uint8Array(buffer, 4, 8); // Start at offset 4, length 8

    strictEqual(view.byteOffset, 4);
    strictEqual(view.byteLength, 8);

    const { value, done } = await reader.read(view);

    ok(!done);
    ok(value instanceof Uint8Array);
    strictEqual(value.byteLength, 8);
    strictEqual(value.buffer.byteLength, 16);

    reader.releaseLock();
  },
};

// Test autoAllocateChunkSize with various values
// Inspired by: Bun test/js/node/test/parallel/test-whatwg-readablebytestreambyob.js
export const byobAutoAllocateSizes = {
  async test() {
    const testSizes = [1, 1024, 65536];

    for (const chunkSize of testSizes) {
      let receivedSize = 0;

      const rs = new ReadableStream({
        type: 'bytes',
        autoAllocateChunkSize: chunkSize,
        pull(controller) {
          if (controller.byobRequest) {
            receivedSize = controller.byobRequest.view.byteLength;
            controller.byobRequest.view[0] = 42;
            controller.byobRequest.respond(1);
            controller.close();
          }
        },
      });

      const reader = rs.getReader();
      const { value } = await reader.read();

      strictEqual(
        receivedSize,
        chunkSize,
        `autoAllocateChunkSize=${chunkSize}`
      );
      ok(value instanceof Uint8Array);

      reader.releaseLock();
    }
  },
};

// A partial BYOB response that does not yet satisfy `atLeast` leaves the read request's
// fill offset at a byte count that is not a multiple of the view's element size, and the
// byobRequest view is rebuilt from that offset. Reading one byte at a time into a
// Uint16Array via readAtLeast() exercises that misaligned rebuild.
export const byobPartialRespondMisalignsFillOffset = {
  async test() {
    let responds = 0;
    const rs = new ReadableStream({
      type: 'bytes',
      pull(controller) {
        const view = controller.byobRequest.view;
        new Uint8Array(view.buffer, view.byteOffset, 1)[0] = ++responds;
        controller.byobRequest.respond(1);
      },
    });

    const reader = rs.getReader({ mode: 'byob' });
    // atLeast is 2 elements == 4 bytes, so this needs four single-byte responses, three
    // of which land on an odd (misaligned) fill offset.
    const { value, done } = await reader.readAtLeast(2, new Uint16Array(3));

    ok(!done);
    ok(value instanceof Uint16Array);
    strictEqual(value.length, 2);
    strictEqual(responds, 4);

    const asBytes = new Uint8Array(
      value.buffer,
      value.byteOffset,
      value.byteLength
    );
    strictEqual(asBytes[0], 1);
    strictEqual(asBytes[1], 2);
    strictEqual(asBytes[2], 3);
    strictEqual(asBytes[3], 4);

    reader.releaseLock();
  },
};

// --- Migrated from streams-js-test.js (byte-only halves) ---
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

// A BYOB read consumes part of an enqueued chunk; a later DEFAULT read
// picks up the remainder (WPT 'enqueue(), read(view) partially, then
// read()'). PARITY: [1,2] to the view, then Uint8Array [3] to the
// default reader.
export const partialViewThenDefaultRead = {
  async test() {
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        c.enqueue(new Uint8Array([1, 2, 3]));
      },
    });
    const byob = rs.getReader({ mode: 'byob' });
    const first = await byob.read(new Uint8Array(2));
    byob.releaseLock();
    const dflt = rs.getReader();
    const second = await Promise.race([
      dflt
        .read()
        .then(
          (r) =>
            `second:done=${r.done},type=${r.value?.constructor?.name},bytes=[${r.value ? Array.from(r.value) : ''}]`
        ),
      scheduler.wait(200).then(() => 'second:pending'),
    ]);
    strictEqual(first.done, false);
    strictEqual(Array.from(first.value).join(','), '1,2');
    strictEqual(second, 'second:done=false,type=Uint8Array,bytes=[3]');
  },
};
