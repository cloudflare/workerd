// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Object.prototype.then pollution while a read settles: the streams
// machinery must neither invoke the interceptor as a thenable nor leak
// it into results, and what the interceptor does to the stream while a
// result settles must not disturb the other consumers of a tee.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const TIMEOUT = Symbol('timeout');
function settled(promise) {
  return Promise.race([promise, scheduler.wait(50).then(() => TIMEOUT)]);
}

// Runs fn with an Object.prototype.then getter installed that calls onFire
// with the object whose `then` is being looked up (a settling read result
// among them) and yields no thenable.
async function withThenGetter(onFire, fn) {
  Object.defineProperty(Object.prototype, 'then', {
    get() {
      onFire(this);
      return undefined;
    },
    configurable: true,
  });
  try {
    await fn();
  } finally {
    delete Object.prototype.then;
  }
}

function teedWithPendingReads(highWaterMark = 1) {
  let controller;
  const rs = new ReadableStream(
    {
      start(c) {
        controller = c;
      },
    },
    { highWaterMark }
  );
  const [a, b] = rs.tee();
  const readerA = a.getReader();
  const readerB = b.getReader();
  return {
    controller,
    readerA,
    readerB,
    readA: readerA.read(),
    readB: readerB.read(),
  };
}

// The getter fires while branch A's read result settles and cancels A. The
// chunk still reaches branch B's pending read, and B's backlog still counts
// toward desiredSize.
export const thenGetterCancelsBranchDuringEnqueue = {
  async test() {
    const { controller, readerA, readA, readB } = teedWithPendingReads();
    let fired = 0;
    await withThenGetter(
      (result) => {
        if (result.value === 'x' && result.done === false && fired++ === 0) {
          readerA.cancel('bye');
        }
      },
      async () => {
        controller.enqueue('x');
        deepStrictEqual(await settled(readA), { value: 'x', done: false });
        deepStrictEqual(await settled(readB), { value: 'x', done: false });
      }
    );
  },
};

export const thenGetterCancelsBranchDuringEnqueueBacklog = {
  async test() {
    let controller;
    const rs = new ReadableStream(
      {
        start(c) {
          controller = c;
        },
      },
      { highWaterMark: 1 }
    );
    const [a, b] = rs.tee();
    const readerA = a.getReader();
    const readA = readerA.read();
    let fired = 0;
    await withThenGetter(
      (result) => {
        if (result.value === 'x' && result.done === false && fired++ === 0) {
          readerA.cancel('bye');
        }
      },
      async () => {
        controller.enqueue('x');
        deepStrictEqual(await settled(readA), { value: 'x', done: false });
        // B has not read: its backlog of one chunk is the slowest consumer's.
        strictEqual(controller.desiredSize, 0);
        deepStrictEqual(await settled(b.getReader().read()), {
          value: 'x',
          done: false,
        });
      }
    );
  },
};

// The same during close(): the sentinel settles A's read as done, the
// getter cancels A, and B's pending read still resolves done.
export const thenGetterCancelsBranchDuringClose = {
  async test() {
    const { controller, readerA, readA, readB } = teedWithPendingReads();
    let fired = 0;
    await withThenGetter(
      (result) => {
        if (result.done === true && fired++ === 0) {
          readerA.cancel('bye');
        }
      },
      async () => {
        controller.close();
        for (const read of [readA, readB]) {
          const result = await settled(read);
          strictEqual(result.done, true);
          strictEqual(result.value, undefined);
        }
      }
    );
  },
};

// Three consumers (B re-teed into C and D): the getter lets A's result pass,
// then cancels both A and C while C's settles, leaving D as the only
// consumer that has not yet been served. D's read still resolves.
export const thenGetterCancelsTwoBranchesDuringEnqueue = {
  async test() {
    let controller;
    const rs = new ReadableStream(
      {
        start(c) {
          controller = c;
        },
      },
      { highWaterMark: 1 }
    );
    const [a, b] = rs.tee();
    const [c, d] = b.tee();
    const readerA = a.getReader();
    const readerC = c.getReader();
    const readerD = d.getReader();
    const readA = readerA.read();
    const readC = readerC.read();
    const readD = readerD.read();
    let fired = 0;
    await withThenGetter(
      (result) => {
        if (result.value === 'x' && result.done === false && ++fired === 2) {
          readerA.cancel('bye');
          readerC.cancel('bye');
        }
      },
      async () => {
        controller.enqueue('x');
        deepStrictEqual(await settled(readA), { value: 'x', done: false });
        deepStrictEqual(await settled(readC), { value: 'x', done: false });
        deepStrictEqual(await settled(readD), { value: 'x', done: false });
      }
    );
  },
};

export const thenGetterFireCountOnRead = {
  async test() {
    let fired = 0;
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const reader = rs.getReader();
    try {
      Object.defineProperty(Object.prototype, 'then', {
        get() {
          fired++;
          return undefined;
        },
        configurable: true,
      });
      const read = reader.read();
      controller.enqueue('x');
      const r = await read;
      strictEqual(r.value, 'x');
      controller.close();
      await reader.closed;
    } finally {
      delete Object.prototype.then;
    }
    strictEqual('then' in {}, false, 'interceptor must be removed');
    // Counts measured in the wd-test harness context (see the transform
    // suite's thenGetterFireCount for the context-sensitivity note).
    strictEqual(fired, usingTsImpl ? 2 : 1);
  },
};
