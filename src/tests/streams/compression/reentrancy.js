// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Re-entrancy edges. Read results are ordinary objects, so resolving a
// read runs the thenable check — a patched Object.prototype.then getter
// fires once per read under C++, twice under TypeScript. A second
// concurrent default read diverges: the C++ internal readable supports a
// single pending read (TypeError), TypeScript parks and serves in order.

import { ok, strictEqual, deepStrictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const enc = new TextEncoder();

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
