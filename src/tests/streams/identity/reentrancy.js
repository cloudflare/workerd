// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Re-entrancy edges: user code that runs in the middle of stream processing
// and re-enters the stream API.
//
// Buffer metadata getters are never consulted (see buffer-lifecycle.js and
// byob.js), but one user hook IS reached from inside read processing in
// both implementations: read results are ordinary objects, so resolving a
// read promise runs the spec's thenable check against them, and a patched
// Object.prototype.then getter executes mid-delivery — C++ consults it once
// per read, TypeScript twice. Whatever such a getter does must not be able
// to corrupt the stream.
//
// The parked-second-read case additionally pins a reader-model divergence
// (ledger #16): C++ identity readables reject a second concurrent default
// read outright, while TypeScript parks it and serves it in order.

import { ok, strictEqual, deepStrictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Installs an Object.prototype.then accessor for the duration of fn().
// The getter returns undefined (a non-thenable answer), so intercepted
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
        // Default read.
        const its = new IdentityTransformStream();
        const writer = its.writable.getWriter();
        const reader = its.readable.getReader();
        const readPromise = reader.read();
        await writer.write(new Uint8Array([97, 98]));
        const r = await readPromise;
        strictEqual(r.done, false);
        deepStrictEqual([...r.value], [97, 98]);
        strictEqual(fired, usingTsImpl ? 2 : 1);

        // BYOB read consults it the same number of times.
        const before = fired;
        const byob = new IdentityTransformStream();
        const w2 = byob.writable.getWriter();
        const rd2 = byob.readable.getReader({ mode: 'byob' });
        const rp2 = rd2.read(new Uint8Array(4));
        await w2.write(new Uint8Array([99, 100]));
        const r2 = await rp2;
        deepStrictEqual([...r2.value], [99, 100]);
        strictEqual(fired - before, usingTsImpl ? 2 : 1);
      }
    );
  },
};

export const closeWriterFromThenInterceptorDuringRead = {
  async test() {
    // The interceptor re-enters the stream: on its first invocation —
    // which happens while the read result is being delivered — it closes
    // the writer. The in-flight read must still deliver its bytes, the
    // write and close must complete, and the stream must end cleanly.
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const reader = its.readable.getReader();
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
        const writePromise = writer.write(new Uint8Array([120, 121]));
        const r = await readPromise;
        strictEqual(r.done, false);
        deepStrictEqual([...r.value], [120, 121]);
        await writePromise;
        strictEqual((await reader.read()).done, true);
        await closePromise;
      }
    );
    ok(fired >= 1, 'the interceptor must have fired');
  },
};

export const closeFromReadContinuationWithSecondReadParked = {
  async test() {
    // Closing the writer from inside a read continuation is safe in both
    // implementations. What happens to a SECOND read issued while the
    // first is still pending diverges (ledger #16):
    // - C++ rejects the second read at read() time: identity readables
    //   support only a single pending read request.
    // - TypeScript parks it and serves it in order — here, the close.
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const reader = its.readable.getReader();
    const first = reader.read().then((r) => {
      writer.close();
      return r;
    });
    const second = reader.read();
    if (usingTsImpl) {
      await writer.write(new Uint8Array([122]));
      const r1 = await first;
      deepStrictEqual([...r1.value], [122]);
      strictEqual((await second).done, true);
    } else {
      await rejects(second, (err) => {
        ok(err instanceof TypeError);
        return /single pending read request/.test(err.message);
      });
      await writer.write(new Uint8Array([122]));
      const r1 = await first;
      deepStrictEqual([...r1.value], [122]);
      // The rejected second read did not damage the stream: EOF arrives.
      strictEqual((await reader.read()).done, true);
    }
  },
};

export const writeFromReadContinuation = {
  async test() {
    // Issuing the next write from inside a read continuation re-enters the
    // rendezvous while it is being resolved; ordering must hold.
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const reader = its.readable.getReader();
    let secondWrite;
    const first = reader.read().then((r) => {
      secondWrite = writer.write(new Uint8Array([50]));
      return r;
    });
    await writer.write(new Uint8Array([49]));
    const r1 = await first;
    deepStrictEqual([...r1.value], [49]);
    const r2 = await reader.read();
    deepStrictEqual([...r2.value], [50]);
    await secondWrite;
    await writer.close();
  },
};

export const cancelSiblingFromReadContinuation = {
  async test() {
    // Cancelling the sibling tee branch from inside the other branch's
    // read continuation must not disturb the surviving branch.
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [a, b] = its.readable.tee();
    const readerA = a.getReader();
    const first = readerA.read().then((r) => {
      b.cancel(new Error('sibling cancelled from continuation'));
      return r;
    });
    await writer.write(new Uint8Array([113]));
    const r1 = await first;
    strictEqual(r1.done, false);
    deepStrictEqual([...r1.value], [113]);
    // The survivor keeps flowing.
    const writePromise = writer.write(new Uint8Array([114]));
    const r2 = await readerA.read();
    deepStrictEqual([...r2.value], [114]);
    await writePromise;
    await writer.close();
    strictEqual((await readerA.read()).done, true);
  },
};
