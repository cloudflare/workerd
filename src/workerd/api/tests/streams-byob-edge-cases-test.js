// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for BYOB reader edge cases with various typed arrays.
// These tests focus on different ArrayBufferView types beyond Uint8Array,
// view offsets, and autoAllocateChunkSize variations.
//
// Test inspirations:
// - Bun: test/js/node/test/parallel/test-whatwg-readablebytestreambyob.js (BYOB reader tests)
// - Bun: test/js/node/test/parallel/test-whatwg-readablebytestream.js (ReadableByteStreamController)
// - Deno: tests/unit_node/_fs/_fs_handle_test.ts (FileHandle BYOB reader)

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
