// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { ok } from 'node:assert';

export const SabViewPipeThroughCompressionStream = {
  async test() {
    const sab = new SharedArrayBuffer(100);
    const view = new Uint8Array(sab);
    view.fill(0x41);

    const readable = new ReadableStream({
      start(controller) {
        controller.enqueue(view);
        controller.close();
      },
    });

    const cs = new CompressionStream('gzip');
    await readable.pipeTo(cs.writable);

    const reader = cs.readable.getReader();
    const { value, done } = await reader.read();
    ok(!done);
    ok(value.byteLength > 0, 'Expected compressed output');
  },
};

export const SabViewPipeThroughIdentityTransform = {
  async test() {
    const sab = new SharedArrayBuffer(1024);
    const view = new Uint8Array(sab);
    view.fill(0x42);

    const readable = new ReadableStream({
      start(controller) {
        controller.enqueue(view);
        controller.close();
      },
    });

    const ts = new IdentityTransformStream();
    const pipePromise = readable.pipeTo(ts.writable);

    const reader = ts.readable.getReader();
    const chunks = [];
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      chunks.push(value);
    }
    await pipePromise;

    const totalLength = chunks.reduce((sum, c) => sum + c.byteLength, 0);
    ok(totalLength === 1024, `Expected 1024 bytes, got ${totalLength}`);
  },
};

export const ResizableBufferPipeThroughIdentityTransform = {
  async test() {
    const buffer = new ArrayBuffer(1024, { maxByteLength: 2048 });
    const view = new Uint8Array(buffer);
    view.fill(0x43);

    const readable = new ReadableStream({
      start(controller) {
        controller.enqueue(view);
        controller.close();
      },
    });

    const ts = new IdentityTransformStream();
    const pipePromise = readable.pipeTo(ts.writable);

    const reader = ts.readable.getReader();
    const chunks = [];
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      chunks.push(value);
    }
    await pipePromise;

    const totalLength = chunks.reduce((sum, c) => sum + c.byteLength, 0);
    ok(totalLength === 1024, `Expected 1024 bytes, got ${totalLength}`);
  },
};
