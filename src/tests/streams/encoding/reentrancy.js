// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Re-entrancy edges: user code that runs in the middle of stream processing
// and re-enters the stream API.
//
// Two user hooks are reachable mid-processing. Read results are ordinary
// objects, so resolving a read runs the thenable check against them and a
// patched Object.prototype.then getter executes mid-delivery — consulted
// once per read under C++, twice under TypeScript (same counts as the
// identity suite). And the encoder ToString-coerces object chunks, so a
// user toString() runs INSIDE the transform — a hook the identity streams
// do not have.
//
// Unlike the identity readables (identity ledger #16), these are standard
// readables in both implementations: a second concurrent read parks and is
// served in order — no divergence.

import { ok, strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Installs an Object.prototype.then accessor for the duration of fn(). The
// getter returns undefined (a non-thenable answer), so intercepted
// resolutions proceed normally after the hook has run.
async function withThenInterceptor(onGet, fn) {
  Object.defineProperty(Object.prototype, 'then', {
    get() {
      onGet();
      return undefined;
    },
    configurable: true,
  });
  try {
    await fn();
  } finally {
    delete Object.prototype.then;
  }
  strictEqual('then' in {}, false, 'interceptor must be removed');
}

export const thenInterceptionDuringReadResolution = {
  async test() {
    let fired = 0;
    await withThenInterceptor(
      () => fired++,
      async () => {
        const tes = new TextEncoderStream();
        const writer = tes.writable.getWriter();
        const reader = tes.readable.getReader();
        const readPromise = reader.read();
        await writer.write('ab');
        deepStrictEqual([...(await readPromise).value], [97, 98]);
        strictEqual(fired, usingTsImpl ? 2 : 1);

        const before = fired;
        const tds = new TextDecoderStream();
        const w2 = tds.writable.getWriter();
        const r2 = tds.readable.getReader();
        const rp2 = r2.read();
        await w2.write(new TextEncoder().encode('cd'));
        strictEqual((await rp2).value, 'cd');
        strictEqual(fired - before, usingTsImpl ? 2 : 1);
      }
    );
  },
};

export const closeWriterFromThenInterceptorDuringRead = {
  async test() {
    // The interceptor re-enters the stream: while the decoded chunk is
    // being delivered it closes the writer. The read still delivers, the
    // write and close complete, and the stream ends cleanly.
    const tds = new TextDecoderStream();
    const writer = tds.writable.getWriter();
    const reader = tds.readable.getReader();
    let fired = 0;
    let closePromise;
    await withThenInterceptor(
      () => {
        if (++fired === 1) {
          closePromise = writer.close();
        }
      },
      async () => {
        const readPromise = reader.read();
        const writePromise = writer.write(new TextEncoder().encode('xy'));
        strictEqual((await readPromise).value, 'xy');
        await writePromise;
        strictEqual((await reader.read()).done, true);
        await closePromise;
      }
    );
    ok(fired >= 1, 'the interceptor must have fired');
  },
};

export const encoderChunkToStringReentersStream = {
  async test() {
    // The user's toString() runs inside the transform. Re-entering with
    // another write and a close from there is safe: the re-entrant write
    // queues behind the chunk being coerced, everything settles, and the
    // stream ends cleanly.
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    const reader = tes.readable.getReader();
    const dec = new TextDecoder();
    const got = [];
    const drained = (async () => {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        got.push(dec.decode(value));
      }
    })();
    let closePromise;
    const chunk = {
      toString() {
        writer.write('reentrant');
        closePromise = writer.close();
        return 'evil';
      },
    };
    await writer.write(chunk);
    await drained;
    deepStrictEqual(got, ['evil', 'reentrant']);
    await closePromise;
  },
};

export const writeFromReadContinuation = {
  async test() {
    // Issuing the next write from inside a read continuation re-enters the
    // transform while its output is being delivered; ordering must hold.
    const tds = new TextDecoderStream();
    const writer = tds.writable.getWriter();
    const reader = tds.readable.getReader();
    const enc = new TextEncoder();
    let secondWrite;
    const first = reader.read().then((r) => {
      secondWrite = writer.write(enc.encode('2'));
      return r;
    });
    await writer.write(enc.encode('1'));
    strictEqual((await first).value, '1');
    strictEqual((await reader.read()).value, '2');
    await secondWrite;
    await writer.close();
  },
};

export const secondConcurrentReadParked = {
  async test() {
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    const reader = tes.readable.getReader();
    const dec = new TextDecoder();
    const first = reader.read();
    const second = reader.read();
    await writer.write('1');
    await writer.write('2');
    strictEqual(dec.decode((await first).value), '1');
    strictEqual(dec.decode((await second).value), '2');
    await writer.close();
    strictEqual((await reader.read()).done, true);
  },
};

export const cancelSiblingFromReadContinuation = {
  async test() {
    // Cancelling the sibling tee branch from inside the other branch's
    // read continuation must not disturb the survivor.
    const tds = new TextDecoderStream();
    const writer = tds.writable.getWriter();
    const enc = new TextEncoder();
    const [a, b] = tds.readable.tee();
    const readerA = a.getReader();
    const first = readerA.read().then((r) => {
      b.cancel(new Error('sibling cancelled from continuation'));
      return r;
    });
    await writer.write(enc.encode('q'));
    strictEqual((await first).value, 'q');
    const writePromise = writer.write(enc.encode('r'));
    strictEqual((await readerA.read()).value, 'r');
    await writePromise;
    await writer.close();
    strictEqual((await readerA.read()).done, true);
  },
};
