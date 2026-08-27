// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Input buffer lifecycle for TextDecoderStream. Unlike the identity
// streams, there is NO write-time snapshot: the chunk is held by reference
// in the writable queue and the transform reads the buffer's CURRENT bytes
// when it runs (on read demand, under readable HWM 0) — in both
// implementations. Mutating after write() changes what decodes; a chunk
// detached while queued contributes nothing. Shadowing metadata getters
// are never consulted (buffer metadata comes from internal slots).

import { strictEqual, deepStrictEqual } from 'node:assert';

export const mutationAfterWriteIsVisible = {
  async test() {
    const enc = new TextEncoder();
    // No demand at write time.
    {
      const tds = new TextDecoderStream();
      const writer = tds.writable.getWriter();
      const view = enc.encode('AAAA');
      const writePromise = writer.write(view);
      view.set(enc.encode('BBBB'));
      const reader = tds.readable.getReader();
      strictEqual((await reader.read()).value, 'BBBB');
      await writePromise;
      await writer.close();
    }
    // Same with a read already parked: the transform still runs after the
    // synchronous mutation.
    {
      const tds = new TextDecoderStream();
      const writer = tds.writable.getWriter();
      const reader = tds.readable.getReader();
      const readPromise = reader.read();
      const view = enc.encode('CCCC');
      const writePromise = writer.write(view);
      view.set(enc.encode('DDDD'));
      strictEqual((await readPromise).value, 'DDDD');
      await writePromise;
      await writer.close();
    }
  },
};

export const shrinkAfterWriteDecodesRemainingBytes = {
  async test() {
    // A length-tracking view over a resizable buffer, shrunk after write:
    // the transform sees the post-shrink extent.
    const rab = new ArrayBuffer(4, { maxByteLength: 8 });
    const view = new Uint8Array(rab);
    view.set(new TextEncoder().encode('FFFF'));
    const tds = new TextDecoderStream();
    const writer = tds.writable.getWriter();
    const writePromise = writer.write(view);
    rab.resize(2);
    const reader = tds.readable.getReader();
    strictEqual((await reader.read()).value, 'FF');
    await writePromise;
    await writer.close();
  },
};

export const detachWhileQueuedContributesNothing = {
  async test() {
    // A chunk detached between write() and the transform decodes as zero
    // bytes: the write and close resolve, the reader sees clean EOF.
    const tds = new TextDecoderStream();
    const writer = tds.writable.getWriter();
    const view = new TextEncoder().encode('EE');
    const writePromise = writer.write(view);
    view.buffer.transfer();
    const reader = tds.readable.getReader();
    const readPromise = reader.read();
    await Promise.all([writePromise, writer.close()]);
    strictEqual((await readPromise).done, true);
  },
};

export const lyingMetadataNeverConsulted = {
  async test() {
    // Buffer metadata is read from internal slots; shadowing own getters
    // are never invoked, whichever side of the write they are installed on.
    const view = new TextEncoder().encode('real');
    for (const key of ['byteLength', 'byteOffset', 'buffer']) {
      Object.defineProperty(view, key, {
        get() {
          throw new Error(`${key} getter must not be called`);
        },
      });
    }
    const tds = new TextDecoderStream();
    const writer = tds.writable.getWriter();
    const writePromise = writer.write(view);
    const reader = tds.readable.getReader();
    deepStrictEqual((await reader.read()).value, 'real');
    await writePromise;
    await writer.close();
  },
};
