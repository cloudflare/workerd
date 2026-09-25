// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructor validation shapes, migrated from streams-js-test.js
// (the WritableStream half of highWaterMarkValidated).
//
// DIVERGENCE: invalid highWaterMark values throw TypeError from the C++
// jsg integer-conversion boundary ("cannot be converted because it is
// negative"/"not an integer") but RangeError("Invalid highWaterMark")
// from the TypeScript implementation, which follows the spec's
// ExtractHighWaterMark. This is the writable-streams/bad-strategies
// WPT failure pair ("We have TypeError, they want RangeError").

import { strictEqual, deepStrictEqual, ok, throws } from 'node:assert';
import { usingTsImpl, pedanticWpt } from 'which-impl';

// Invalid highWaterMark values are rejected at construction; the error
// type diverges per implementation.
export const highWaterMarkValidated = {
  test() {
    const expected = usingTsImpl ? RangeError : TypeError;
    [-1, -Infinity, NaN, {}, 'foo'].forEach((highWaterMark) => {
      throws(() => new WritableStream(undefined, { highWaterMark }), expected);
    });
    // Fractional values pass validation in both implementations (their
    // effect on accounting is pinned in backpressure.js and, for the
    // strategy classes, the strategies suite).
    new WritableStream(undefined, { highWaterMark: 0.5 });
    // Coercing objects go through ToNumber in both implementations.
    const ws = new WritableStream(undefined, {
      highWaterMark: {
        valueOf() {
          return 2;
        },
      },
    });
    strictEqual(ws.getWriter().desiredSize, 2);
  },
};

// DIVERGENCE: the spec requires underlyingSink.type to be undefined
// (RangeError otherwise). The TypeScript implementation enforces this;
// C++ ignores the property entirely unless pedantic_wpt is set
// (standard.c++ WritableStreamJsController::setup).
export const sinkTypeValidation = {
  test() {
    for (const type of ['bytes', null, '']) {
      if (usingTsImpl || pedanticWpt) {
        throws(() => new WritableStream({ type }), RangeError);
      } else {
        new WritableStream({ type });
      }
    }
    // undefined is valid everywhere.
    new WritableStream({ type: undefined });
  },
};

// DIVERGENCE: the spec converts the queuingStrategy argument before
// inspecting the underlying sink (the WPT constructor.any "converted
// after queuingStrategy" case). TypeScript reads strategy.size, then
// strategy.highWaterMark (twice: validation and storage), then
// sink.write. C++ converts the sink dictionary first.
export const argumentConversionOrder = {
  test() {
    const order = [];
    const sink = {};
    const strategy = {};
    Object.defineProperty(sink, 'write', {
      get() {
        order.push('sink.write');
        return undefined;
      },
    });
    Object.defineProperty(strategy, 'size', {
      get() {
        order.push('strategy.size');
        return undefined;
      },
    });
    Object.defineProperty(strategy, 'highWaterMark', {
      get() {
        order.push('strategy.highWaterMark');
        return 1;
      },
    });
    new WritableStream(sink, strategy);
    if (usingTsImpl) {
      deepStrictEqual(order, [
        'strategy.size',
        'strategy.highWaterMark',
        'strategy.highWaterMark',
        'sink.write',
      ]);
    } else {
      deepStrictEqual(order, [
        'sink.write',
        'strategy.highWaterMark',
        'strategy.size',
      ]);
    }
  },
};

// DIVERGENCE: with no backpressure the spec starts the stream with an
// already-resolved ready promise (the WPT constructor.any "ready should
// fulfill immediately" case). TypeScript's ready is fulfilled within a
// microtask of construction; C++ resolves it only on a later turn. Both
// report desiredSize 1 immediately.
export const readyFulfillTiming = {
  async test() {
    const ws = new WritableStream();
    const writer = ws.getWriter();
    strictEqual(writer.desiredSize, 1);
    let state = 'pending';
    writer.ready.then(
      () => (state = 'fulfilled'),
      () => (state = 'rejected')
    );
    await Promise.resolve();
    strictEqual(state, usingTsImpl ? 'fulfilled' : 'pending');
    // Both eventually fulfill.
    await writer.ready;
    ok(true);
  },
};

// A non-callable strategy.size throws TypeError at construction in both
// implementations, with per-implementation messages (jsg dictionary
// boundary vs the TypeScript validator).
export const nonCallableSizeThrows = {
  test() {
    for (const size of [42, 'x']) {
      throws(() => new WritableStream(undefined, { size, highWaterMark: 1 }), {
        name: 'TypeError',
        message: usingTsImpl
          ? 'strategy.size must be a function'
          : "Incorrect type for the 'size' field on 'StreamQueuingStrategy': the provided value is not of type 'function'.",
      });
    }
  },
};

// A JS-backed WritableStream can be created and consumed at the global scope
const globalPipe = (async () => {
  const chunks = [];
  const rs = new ReadableStream({
    start(c) {
      c.enqueue('hello');
      c.close();
    },
  });
  const ws = new WritableStream({
    write(c) {
      chunks.push(c);
    },
  });
  await rs.pipeTo(ws);
  return chunks.join('');
})();

export const globalScopePipe = {
  async test() {
    strictEqual(await globalPipe, 'hello');
  },
};
