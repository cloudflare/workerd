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
    strictEqual(fired, 1);
  },
};

// Counts the then-getter fires on read results while op runs, and checks
// that every result op returns is an ordinary object. A result is an object
// with an own `done` (a C++ end-of-stream result has no `value`).
async function resultThenLookups(op) {
  let fired = 0;
  let results;
  await withThenGetter(
    (target) => {
      if (Object.hasOwn(target, 'done')) {
        fired++;
      }
    },
    async () => {
      results = await op();
    }
  );
  for (const result of results ?? []) {
    strictEqual(Object.getPrototypeOf(result), Object.prototype);
  }
  return fired;
}

function pushSource(highWaterMark = 1) {
  let controller;
  const rs = new ReadableStream(
    {
      start(c) {
        controller = c;
      },
    },
    { highWaterMark }
  );
  return { rs, controller };
}

// Resolving the promise read() returns looks `then` up on its result once,
// whether the read is answered from the queue, waits for enqueue() or
// close(), or finds the stream closed; a tee branch's read too.
export const thenGetterFiresOncePerRead = {
  async test() {
    const cases = {
      queued() {
        const { rs, controller } = pushSource();
        controller.enqueue('x');
        return async () => [await rs.getReader().read()];
      },
      waitsForEnqueue() {
        const { rs, controller } = pushSource();
        const reader = rs.getReader();
        return async () => {
          const read = reader.read();
          controller.enqueue('x');
          return [await read];
        };
      },
      waitsForClose() {
        const { rs, controller } = pushSource();
        const reader = rs.getReader();
        return async () => {
          const read = reader.read();
          controller.close();
          return [await read];
        };
      },
      lastChunkWithCloseRequested() {
        const { rs, controller } = pushSource();
        controller.enqueue('x');
        controller.close();
        return async () => [await rs.getReader().read()];
      },
      closed() {
        const { rs, controller } = pushSource();
        controller.close();
        return async () => [await rs.getReader().read()];
      },
      teeBranchWaitsForEnqueue() {
        const { rs, controller } = pushSource();
        const [branch] = rs.tee();
        const reader = branch.getReader();
        return async () => {
          const read = reader.read();
          controller.enqueue('x');
          return [await read];
        };
      },
    };
    for (const [name, setup] of Object.entries(cases)) {
      const op = setup();
      const fired = await resultThenLookups(op);
      strictEqual(fired, 1, `${name}: ${fired}`);
    }
  },
};

// When the getter runs for a read waiting on enqueue() or close(): inside
// the call, as the call resolves the read promise (spec; Node agrees),
// versus once the call has returned (C++; ledger #16).
export const thenGetterTimingForWaitingRead = {
  async test() {
    for (const end of ['enqueue', 'close']) {
      const { rs, controller } = pushSource();
      const reader = rs.getReader();
      const read = reader.read();
      const events = [];
      await withThenGetter(
        (target) => {
          if (Object.hasOwn(target, 'done')) events.push('getter');
        },
        async () => {
          if (end === 'enqueue') controller.enqueue('x');
          else controller.close();
          events.push('returned');
          await read;
        }
      );
      deepStrictEqual(
        events,
        usingTsImpl ? ['getter', 'returned'] : ['returned', 'getter'],
        end
      );
    }
  },
};

// A pipe's reads settle internal promises only: the getter never sees a
// read result, whether the pipe finds chunks queued or waits for them.
export const thenGetterNotConsultedByPipeReads = {
  async test() {
    const queued = pushSource();
    for (const chunk of ['a', 'b']) queued.controller.enqueue(chunk);
    queued.controller.close();
    const waiting = pushSource();
    const got = [];
    const sink = () =>
      new WritableStream({
        write(chunk) {
          got.push(chunk);
        },
      });
    const fired = await resultThenLookups(async () => {
      await queued.rs.pipeTo(sink());
      const pipe = waiting.rs.pipeTo(sink());
      await scheduler.wait(0);
      waiting.controller.enqueue('c');
      await scheduler.wait(0);
      waiting.controller.enqueue('d');
      waiting.controller.close();
      await pipe;
    });
    deepStrictEqual(got, ['a', 'b', 'c', 'd']);
    strictEqual(fired, 0);
  },
};

// An async-iterator next() made while no other is pending looks `then` up
// once, answered from the queue or waiting for a chunk. A next() made while
// another is still pending settles by adopting the promise of the step it
// waited for, which looks `then` up a second time (WebIDL; Node agrees);
// C++ looks it up once (ledger #24). The TypeScript first next() is always
// one of those.
export const thenGetterPerIteratorNext = {
  async test() {
    const primed = async (chunks) => {
      const { rs, controller } = pushSource(chunks.length + 1);
      for (const chunk of chunks) controller.enqueue(chunk);
      const it = rs.values();
      await it.next();
      return { it, controller };
    };
    {
      const { it } = await primed(['x', 'y']);
      strictEqual(await resultThenLookups(async () => [await it.next()]), 1);
    }
    {
      const { it, controller } = await primed(['x']);
      const fired = await resultThenLookups(async () => {
        const next = it.next();
        await scheduler.wait(0);
        controller.enqueue('y');
        return [await next];
      });
      strictEqual(fired, 1);
    }
    const chained = usingTsImpl ? 2 : 1;
    {
      const { it } = await primed(['x', 'y', 'z']);
      const fired = await resultThenLookups(async () => {
        const first = it.next();
        const second = it.next();
        return [await first, await second];
      });
      strictEqual(fired, 1 + chained);
    }
    {
      const { rs, controller } = pushSource();
      controller.enqueue('x');
      const it = rs.values();
      strictEqual(
        await resultThenLookups(async () => [await it.next()]),
        chained
      );
    }
  },
};
