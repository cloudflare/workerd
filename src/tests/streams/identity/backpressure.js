// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Writer-side backpressure signaling: desiredSize and ready. Identity
// streams count backpressure in BYTES (not chunks), including bytes still in
// flight, and recover as reads consume them.
//
// String chunk accounting deliberately diverges between implementations and
// is asserted per implementation below: C++ counts the exact encoded UTF-8
// byte length; TypeScript uses a conservative length*3 upper-bound estimate
// (the maximum UTF-8 bytes per UTF-16 code unit). Both feed only
// backpressure signaling, where overcounting is harmless.

import { strictEqual, notStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const queuedWritesAndCloseBufferUntilRead = {
  async test() {
    // Multiple writes and a close may all be queued before any read: the
    // chunks buffer inside the stream, in order. Only the promise
    // settlement waits for consumption. The highWaterMark budget is
    // advisory, not enforced: writes beyond it are still accepted, with
    // desiredSize going negative to report the deficit.
    const { readable, writable } = new IdentityTransformStream({
      highWaterMark: 2,
    });
    const writer = writable.getWriter();
    const enc = new TextEncoder();
    const pending = [
      writer.write(enc.encode('a')),
      writer.write(enc.encode('b')),
      writer.write(enc.encode('c')),
      writer.close(),
    ];
    // 3 bytes buffered against a budget of 2.
    strictEqual(writer.desiredSize, -1);
    // Nothing settles before reads: yield through the event loop and check.
    let settled = 0;
    for (const p of pending) {
      p.then(() => settled++);
    }
    await scheduler.wait(0);
    strictEqual(settled, 0);
    // Reads drain the buffered chunks in order, then see EOF; only then do
    // the writes and the close settle.
    const reader = readable.getReader();
    const dec = new TextDecoder();
    for (const expected of ['a', 'b', 'c']) {
      const { value, done } = await reader.read();
      strictEqual(done, false);
      strictEqual(dec.decode(value), expected);
    }
    strictEqual((await reader.read()).done, true);
    await Promise.all(pending);
    strictEqual(settled, 4);
  },
};

export const defaultHighWaterMarkIsOne = {
  test() {
    const its = new IdentityTransformStream();
    strictEqual(its.writable.getWriter().desiredSize, 1);
    const fls = new FixedLengthStream(10);
    strictEqual(fls.writable.getWriter().desiredSize, 1);
  },
};

export const defaultHighWaterMarkAccounting = {
  async test() {
    // Without an explicit highWaterMark the implementations account
    // differently (ledger #17):
    // - C++ installs no accounting at all: desiredSize stays 1 no matter
    //   how many writes are buffered, and ready is never replaced.
    // - TypeScript counts one unit per buffered chunk against the default
    //   highWaterMark of 1, so desiredSize dips negative and ready is
    //   replaced until reads drain the queue.
    // Both converge again once everything is consumed.
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const reader = its.readable.getReader();
    strictEqual(writer.desiredSize, 1);
    const firstReady = writer.ready;
    await writer.ready;
    const w1 = writer.write(new Uint8Array(1));
    strictEqual(writer.desiredSize, usingTsImpl ? 0 : 1);
    const w2 = writer.write(new Uint8Array(9));
    strictEqual(writer.desiredSize, usingTsImpl ? -1 : 1);
    if (usingTsImpl) {
      notStrictEqual(firstReady, writer.ready);
    } else {
      strictEqual(firstReady, writer.ready);
    }
    await reader.read();
    strictEqual(writer.desiredSize, usingTsImpl ? 0 : 1);
    await reader.read();
    strictEqual(writer.desiredSize, 1);
    await writer.ready;
    await Promise.all([w1, w2]);
    await writer.close();
  },
};

export const explicitHighWaterMarkIsInitialDesiredSize = {
  test() {
    const { writable } = new IdentityTransformStream({ highWaterMark: 10 });
    strictEqual(writable.getWriter().desiredSize, 10);

    // A negative-zero highWaterMark is normalized to +0 in both
    // implementations (C++ via uint64 coercion, TypeScript explicitly), so
    // it cannot surface as a negative-zero desiredSize. strictEqual is
    // SameValue and would catch a leaked -0.
    strictEqual(
      new IdentityTransformStream({ highWaterMark: -0 }).writable.getWriter()
        .desiredSize,
      0
    );
    strictEqual(
      new FixedLengthStream(5, { highWaterMark: -0 }).writable.getWriter()
        .desiredSize,
      0
    );
  },
};

export const desiredSizeTracksBytes = {
  async test() {
    const { readable, writable } = new IdentityTransformStream({
      highWaterMark: 20,
    });
    const writer = writable.getWriter();
    const reader = readable.getReader();

    strictEqual(writer.desiredSize, 20);

    // Writes stay counted in desiredSize until fully consumed.
    const w1 = writer.write(new Uint8Array(5));
    strictEqual(writer.desiredSize, 15);
    const w2 = writer.write(new Uint8Array(5));
    strictEqual(writer.desiredSize, 10);
    const w3 = writer.write(new Uint8Array(7));
    strictEqual(writer.desiredSize, 3);

    // Drain: as reads consume writes, desiredSize recovers.
    await reader.read();
    await reader.read();
    await reader.read();
    strictEqual(writer.desiredSize, 20);

    await Promise.all([w1, w2, w3]);
    await writer.close();
  },
};

export const stringWriteDesiredSizeAccounting = {
  async test() {
    const { readable, writable } = new IdentityTransformStream({
      highWaterMark: 100,
    });
    const writer = writable.getWriter();
    const reader = readable.getReader();

    strictEqual(writer.desiredSize, 100);

    // "hello" is 5 code units, all ASCII: 5 actual UTF-8 bytes.
    // C++ counts the exact 5; TypeScript estimates 5 * 3 = 15.
    const w = writer.write('hello');
    strictEqual(writer.desiredSize, usingTsImpl ? 85 : 95);

    // Either way, the accounting reverses fully once the chunk is consumed.
    await reader.read();
    strictEqual(writer.desiredSize, 100);

    await w;
    await writer.close();
  },
};

export const readyReflectsBackpressure = {
  async test() {
    const its = new IdentityTransformStream({ highWaterMark: 10 });
    const writer = its.writable.getWriter();
    const reader = its.readable.getReader();

    strictEqual(writer.desiredSize, 10);

    // Wait for writer.ready so the stream is fully started.
    const firstReady = writer.ready;
    await writer.ready;

    const w1 = writer.write(new Uint8Array(1));
    strictEqual(writer.desiredSize, 9);

    // Fill the remaining budget completely.
    const w2 = writer.write(new Uint8Array(9));
    strictEqual(writer.desiredSize, 0);

    // Backpressure is on: the ready promise must have been replaced.
    notStrictEqual(firstReady, writer.ready);

    async function waitForReady() {
      strictEqual(writer.desiredSize, 0);
      await writer.ready;
      // Ready fired after the first (1-byte) chunk was consumed.
      strictEqual(writer.desiredSize, 1);
    }

    await Promise.all([waitForReady(), reader.read()]);

    // Drain the remaining 9 bytes.
    await reader.read();
    strictEqual(writer.desiredSize, 10);

    await Promise.all([w1, w2]);
    await writer.close();
  },
};
