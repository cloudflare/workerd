// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Re-entrancy edges. Read results are ordinary objects, so resolving a
// read runs the thenable check — a patched Object.prototype.then getter
// fires once per read under C++, twice under TypeScript. Under TypeScript
// the first firing runs inside the write that produced the chunk, while it
// is still moving the rest of its output into the readable, so the getter
// can tear the pair down mid-write. A second concurrent default read
// diverges: the C++ internal readable supports a single pending read
// (TypeError), TypeScript parks and serves in order.

import { ok, strictEqual, deepStrictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { pump } from 'round-trip';

const enc = new TextEncoder();

function macrotask() {
  return new Promise((resolve) => setTimeout(resolve, 0));
}

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
        const cs = new CompressionStream('gzip');
        const writer = cs.writable.getWriter();
        const reader = cs.readable.getReader();
        const readPromise = reader.read();
        await writer.write(enc.encode('ab'));
        ok(!(await readPromise).done);
        strictEqual(fired, usingTsImpl ? 2 : 1);
      }
    );
  },
};

export const cancelFromReadResultThenGetterDuringWrite = {
  async test() {
    // A 1 MiB member: under TypeScript the write moves its output into the
    // readable as sixteen 64 KiB pieces, and the getter fires on the first
    // of them, with fifteen still to go. The cancel from there tears the
    // pair down mid-write: the write rejects with the cancel reason (never
    // with an error from enqueuing into the cancelled readable), the read
    // whose result fired the getter still gets its piece, the readable
    // closes and the writable errors with the reason (#13). Under C++ the
    // read resolves from the C++ side once the write has settled: the
    // write resolves, and the cancel leaves the writable untouched (#13).
    const compressed = await pump(new CompressionStream('gzip'), [
      new Uint8Array(1024 * 1024),
    ]);
    const ds = new DecompressionStream('gzip');
    const reader = ds.readable.getReader();
    const writer = ds.writable.getWriter();
    const first = reader.read();
    const reason = new Error('cancelled from then');
    let cancelled;
    let writeOutcome;
    let firstResult;
    await withThenInterceptor(
      () => {
        cancelled ??= reader.cancel(reason);
      },
      async () => {
        writeOutcome = await writer.write(compressed).then(
          () => 'resolved',
          (e) => e
        );
        firstResult = await first;
      }
    );
    ok(cancelled !== undefined, 'the getter fired');
    await cancelled;
    strictEqual(firstResult.done, false);
    ok(firstResult.value.byteLength > 0);
    await reader.closed;
    strictEqual((await reader.read()).done, true);
    if (usingTsImpl) {
      strictEqual(writeOutcome, reason);
      await rejects(writer.closed, (e) => e === reason);
      await rejects(writer.write(enc.encode('x')), (e) => e === reason);
    } else {
      strictEqual(writeOutcome, 'resolved');
      let state = 'pending';
      writer.closed.then(
        () => (state = 'resolved'),
        () => (state = 'rejected')
      );
      await macrotask();
      await macrotask();
      strictEqual(state, 'pending');
    }
  },
};

export const secondConcurrentRead = {
  async test() {
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    const reader = cs.readable.getReader();
    const first = reader.read();
    if (usingTsImpl) {
      const second = reader.read();
      await writer.write(enc.encode('hello'));
      await writer.close();
      strictEqual((await first).done, false);
      // The parked second read is served in order (here: the next chunk
      // or EOF, depending on how the flush batches).
      ok(typeof (await second).done === 'boolean');
    } else {
      await rejects(reader.read(), (err) => {
        strictEqual(err.constructor, TypeError);
        return /single pending read request/.test(err.message);
      });
      await writer.write(enc.encode('hello'));
      await writer.close();
      strictEqual((await first).done, false);
    }
  },
};

export const writeFromReadContinuation = {
  async test() {
    // Issuing the next write from inside a read continuation re-enters the
    // machinery mid-delivery; everything settles and content is intact.
    const cs = new CompressionStream('gzip');
    const ds = new DecompressionStream('gzip');
    const writer = cs.writable.getWriter();
    const reader = cs.readable.getReader();
    let closePromise;
    const first = reader.read().then((r) => {
      closePromise = writer.close();
      return r;
    });
    await writer.write(enc.encode('reenter'));
    const chunks = [(await first).value];
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
    }
    await closePromise;
    // Round-trip to prove integrity.
    const w2 = ds.writable.getWriter();
    const collected = [];
    const drained = (async () => {
      for await (const chunk of ds.readable) {
        collected.push(...chunk);
      }
    })();
    for (const chunk of chunks) {
      await w2.write(chunk);
    }
    await w2.close();
    await drained;
    deepStrictEqual(
      new TextDecoder().decode(new Uint8Array(collected)),
      'reenter'
    );
  },
};

export const cancelSiblingFromReadContinuation = {
  async test() {
    // Cancelling the sibling tee branch from inside the other branch's
    // read continuation must not disturb the survivor.
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    await writer.write(enc.encode('t'));
    await writer.close();
    const [a, b] = cs.readable.tee();
    const readerA = a.getReader();
    const first = readerA.read().then((r) => {
      b.cancel(new Error('sibling cancelled from continuation'));
      return r;
    });
    strictEqual((await first).done, false);
    // The survivor drains to EOF.
    for (;;) {
      const { done } = await readerA.read();
      if (done) break;
    }
  },
};
