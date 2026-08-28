// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Byte halves of the mixed value+byte tests from streams-js-test.js
// (their value halves live in the readable suite), plus small boundary
// pins between the two reader modes.

import { strictEqual, ok, throws, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// A value (non-byte) stream refuses BYOB readers (migrated
// readableStreamDefaultNoByob).
export const defaultStreamNoByobReader = {
  test() {
    const rs = new ReadableStream();
    throws(() => rs.getReader({ mode: 'byob' }), TypeError);
    throws(() => new ReadableStreamBYOBReader(rs), TypeError);
  },
};

// closed fulfills once the final chunk is read, for both reader modes
// (byte halves of readableStreamDefaultClosePromise).
export const closedPromiseByteReaders = {
  async test() {
    // Default reader.
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
    // BYOB reader.
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
      await r.read(new Uint8Array(1));
      await scheduler.wait(1);
      ok(closed);
    }
  },
};

// reader.cancel() resolves pending reads done=true for default, BYOB,
// and readAtLeast reads (byte halves of readableStreamCancelReads).
export const cancelPendingReadsByteReaders = {
  async test() {
    {
      const rs = new ReadableStream({ type: 'bytes' });
      const reader = rs.getReader();
      const read = reader.read();
      reader.cancel();
      const { done } = await read;
      ok(done);
    }
    {
      const rs = new ReadableStream({ type: 'bytes' });
      const reader = rs.getReader({ mode: 'byob' });
      const read = reader.read(new Uint8Array(1));
      reader.cancel();
      const { done } = await read;
      ok(done);
    }
    {
      const rs = new ReadableStream({ type: 'bytes' });
      const reader = rs.getReader({ mode: 'byob' });
      const read = reader.readAtLeast(1, new Uint8Array(1));
      reader.cancel();
      const { done } = await read;
      ok(done);
    }
  },
};

// While a byte stream is locked, getReader/tee/pipeTo/pipeThrough all
// refuse (byte halves of readableStreamReleaseLock; pipeTo rejects
// rather than throws under the pinned capture_async_api_throws in the
// C++ cells, and unconditionally in TypeScript).
export const lockedByteStreamOpsThrow = {
  async test() {
    for (const mode of [undefined, 'byob']) {
      const rs = new ReadableStream({ type: 'bytes' });
      const reader = rs.getReader(mode ? { mode } : undefined);
      throws(() => rs.getReader(), TypeError);
      throws(() => rs.tee(), TypeError);
      await rejects(rs.pipeTo(), TypeError);
      throws(() => rs.pipeThrough(), TypeError);
      reader.releaseLock();
      rs.getReader().releaseLock();
    }
  },
};

// Byte-stream desiredSize accounting: decremented by byteLength on
// enqueue and credited back as reads drain. DIVERGENCE in default-read
// delivery: C++ COALESCES all queued chunks into a single read (5
// bytes), TypeScript delivers chunk-by-chunk (3, then 2). Both restore
// the full high-water mark once drained.
export const byteDesiredSizeAccounting = {
  async test() {
    let controller;
    const rs = new ReadableStream(
      {
        type: 'bytes',
        start(c) {
          controller = c;
        },
      },
      { highWaterMark: 8 }
    );
    strictEqual(controller.desiredSize, 8);
    controller.enqueue(new Uint8Array(3));
    strictEqual(controller.desiredSize, 5);
    controller.enqueue(new Uint8Array(2));
    strictEqual(controller.desiredSize, 3);
    const reader = rs.getReader();
    const r1 = await reader.read();
    if (usingTsImpl) {
      strictEqual(r1.value.byteLength, 3);
      strictEqual(controller.desiredSize, 6);
      const r2 = await reader.read();
      strictEqual(r2.value.byteLength, 2);
    } else {
      strictEqual(r1.value.byteLength, 5);
    }
    strictEqual(controller.desiredSize, 8);
    controller.close();
    const end = await reader.read();
    strictEqual(end.done, true);
  },
};
