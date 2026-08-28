// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Pipes carrying views over SPECIAL buffers: SharedArrayBuffer-backed
// views (non-transferable by definition) and resizable-ArrayBuffer
// views, written through native and JS-backed endpoints (migrated from
// pipe-write-special-buffer-test.js, strengthened from length checks
// to content verification).

import { strictEqual, ok, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function filledSabView(length, byte) {
  const sab = new SharedArrayBuffer(length);
  const view = new Uint8Array(sab);
  view.fill(byte);
  return view;
}

function streamOf(view) {
  return new ReadableStream({
    start(controller) {
      controller.enqueue(view);
      controller.close();
    },
  });
}

async function drainToBytes(readable) {
  const reader = readable.getReader();
  const parts = [];
  let total = 0;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    parts.push(value);
    total += value.byteLength;
  }
  reader.releaseLock();
  const out = new Uint8Array(total);
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.byteLength;
  }
  return out;
}

function assertAllBytes(bytes, expectedLength, expectedByte) {
  strictEqual(bytes.byteLength, expectedLength);
  for (let i = 0; i < bytes.byteLength; i++) {
    if (bytes[i] !== expectedByte) {
      strictEqual(bytes[i], expectedByte, `byte ${i} corrupted`);
    }
  }
}

// A SAB-backed view piped through CompressionStream. DIVERGENCE: C++
// copies the shared bytes and round-trips them; the TypeScript
// CompressionStream write path REJECTS SharedArrayBuffer-backed views
// ('The provided value is not of type (ArrayBuffer or
// ArrayBufferView)') even though its identity stream accepts them (see
// the next test). The shared buffer is untouched either way.
export const sabViewThroughCompressionRoundTrip = {
  async test() {
    const view = filledSabView(100, 0x41);
    const compressed = streamOf(view).pipeThrough(
      new CompressionStream('gzip')
    );
    if (usingTsImpl) {
      await rejects(
        drainToBytes(compressed.pipeThrough(new DecompressionStream('gzip'))),
        {
          name: 'TypeError',
          message:
            'The provided value is not of type (ArrayBuffer or ArrayBufferView)',
        }
      );
    } else {
      const restored = await drainToBytes(
        compressed.pipeThrough(new DecompressionStream('gzip'))
      );
      assertAllBytes(restored, 100, 0x41);
    }
    // The shared buffer itself must be untouched (it cannot be
    // detached).
    assertAllBytes(view, 100, 0x41);
  },
};

// A SAB-backed view piped into a native identity stream.
export const sabViewThroughIdentityTransform = {
  async test() {
    const view = filledSabView(1024, 0x42);
    const its = new IdentityTransformStream();
    const [bytes] = await Promise.all([
      drainToBytes(its.readable),
      streamOf(view).pipeTo(its.writable),
    ]);
    assertAllBytes(bytes, 1024, 0x42);
    assertAllBytes(view, 1024, 0x42);
  },
};

// A SAB-backed view through a JS-backed TransformStream into a JS
// sink: the JS pipe path must carry the view without detaching its
// (non-detachable) buffer.
export const sabViewThroughJsPipeChain = {
  async test() {
    const view = filledSabView(512, 0x44);
    const received = [];
    await streamOf(view)
      .pipeThrough(new TransformStream())
      .pipeTo(
        new WritableStream({
          write(chunk) {
            received.push(chunk);
          },
        })
      );
    strictEqual(received.length, 1);
    assertAllBytes(received[0], 512, 0x44);
    assertAllBytes(view, 512, 0x44);
    // The delivered chunk is the very view (no copy on the JS path).
    strictEqual(received[0], view);
  },
};

// A resizable-ArrayBuffer view piped into a native identity stream.
export const resizableViewThroughIdentityTransform = {
  async test() {
    const buffer = new ArrayBuffer(1024, { maxByteLength: 2048 });
    const view = new Uint8Array(buffer);
    view.fill(0x43);
    const its = new IdentityTransformStream();
    const [bytes] = await Promise.all([
      drainToBytes(its.readable),
      streamOf(view).pipeTo(its.writable),
    ]);
    assertAllBytes(bytes, 1024, 0x43);
  },
};

// A resizable-ArrayBuffer view through the JS pipe path: delivered
// intact, and the buffer remains resizable afterwards.
export const resizableViewThroughJsPipeChain = {
  async test() {
    const buffer = new ArrayBuffer(256, { maxByteLength: 512 });
    const view = new Uint8Array(buffer);
    view.fill(0x45);
    const received = [];
    await streamOf(view)
      .pipeThrough(new TransformStream())
      .pipeTo(
        new WritableStream({
          write(chunk) {
            received.push(chunk);
          },
        })
      );
    strictEqual(received.length, 1);
    assertAllBytes(received[0], 256, 0x45);
    ok(!buffer.detached);
    buffer.resize(512); // still resizable
    strictEqual(buffer.byteLength, 512);
  },
};
