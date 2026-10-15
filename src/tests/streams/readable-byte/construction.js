// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Byte-stream construction: type/strategy validation and defaults.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const outOfIntRange = {
  name: 'TypeError',
  message:
    'Value out of range. Must be between -2147483648 and 2147483647 (inclusive).',
};

// DIVERGENCE (the WPT general.any seed): a size() in the strategy of a
// byte stream is rejected by TypeScript (spec: RangeError) but silently
// accepted — and ignored — by C++. TypeScript rejects it before checking
// highWaterMark, as the spec does; C++ converts an invalid highWaterMark
// to an integer first and throws TypeError.
export const sizeStrategyForBytes = {
  test() {
    for (const highWaterMark of [4, -1]) {
      const construct = () =>
        new ReadableStream(
          { type: 'bytes' },
          {
            size() {
              return 1;
            },
            highWaterMark,
          }
        );
      if (usingTsImpl) {
        throws(construct, {
          name: 'RangeError',
          message: 'The strategy for a byte stream cannot have a size function',
        });
      } else if (highWaterMark < 0) {
        throws(construct, {
          name: 'TypeError',
          message:
            'The value cannot be converted because it is negative and this API expects a positive bigint.',
        });
      } else {
        construct();
      }
    }
  },
};

// DIVERGENCE: autoAllocateChunkSize conversion. Zero, a negative number,
// NaN, a non-numeric string and ±Infinity throw TypeError on both sides
// (different messages), and a fraction truncates in both, so -0.5 is zero
// (and fractionalAutoAllocateChunkSize). C++ converts the member as an
// int, so 2 ** 31 and up throw. TypeScript converts it as the spec's
// [EnforceRange] unsigned long long and then rejects zero, so it accepts
// up to 2 ** 53 - 1 and throws from 2 ** 53.
export const autoAllocateChunkSizeValidated = {
  test() {
    const notZero = 'autoAllocateChunkSize cannot be zero';
    const outOfRange =
      'autoAllocateChunkSize must be a finite number from 0 to 2^53 - 1';
    for (const [bad, message] of [
      [0, notZero],
      [-0.5, notZero],
      [-1, outOfRange],
      [NaN, outOfRange],
      ['a', outOfRange],
      [Infinity, outOfRange],
      [-Infinity, outOfRange],
    ]) {
      throws(
        () => new ReadableStream({ type: 'bytes', autoAllocateChunkSize: bad }),
        {
          name: 'TypeError',
          message: usingTsImpl
            ? message
            : 'The autoAllocateChunkSize option cannot be zero.',
        }
      );
    }
    new ReadableStream({ type: 'bytes', autoAllocateChunkSize: 2 ** 31 - 1 });
    for (const big of [2 ** 31, 2 ** 53 - 1]) {
      const construct = () =>
        new ReadableStream({ type: 'bytes', autoAllocateChunkSize: big });
      if (usingTsImpl) {
        construct();
      } else {
        throws(construct, outOfIntRange);
      }
    }
    for (const big of [2 ** 53, 2 ** 64]) {
      throws(
        () => new ReadableStream({ type: 'bytes', autoAllocateChunkSize: big }),
        usingTsImpl ? { name: 'TypeError', message: outOfRange } : outOfIntRange
      );
    }
  },
};

// (no divergence) A fractional autoAllocateChunkSize truncates, as the
// spec's [EnforceRange] conversion does: 1.5 and 3.9 give a default
// read's byobRequest 1- and 3-byte views.
export const fractionalAutoAllocateChunkSize = {
  async test() {
    for (const [autoAllocateChunkSize, viewSize] of [
      [1.5, 1],
      [3.9, 3],
    ]) {
      let seen;
      const rs = new ReadableStream({
        type: 'bytes',
        autoAllocateChunkSize,
        pull(c) {
          const { view } = c.byobRequest;
          seen = view.byteLength;
          view[0] = 7;
          c.byobRequest.respond(1);
        },
      });
      const { value } = await rs.getReader().read();
      strictEqual(seen, viewSize);
      deepStrictEqual([...value], [7]);
    }
  },
};

// DIVERGENCE: argument conversion order on a byte source, every member
// logged (the readable suite's argumentConversionOrder, ledger #2, for a
// default source). The spec (TypeScript) converts the strategy first
// (highWaterMark and its ToNumber, then size), then the source; C++
// converts the source first, then the strategy in the same order. C++
// reads every member of the source in its own order; TypeScript reads
// type, then the byte controller's members in lexicographic order. The
// spec reads every member in lexicographic order, type last.
export const argumentConversionOrder = {
  test() {
    const order = [];
    const source = {};
    for (const name of [
      'type',
      'autoAllocateChunkSize',
      'start',
      'pull',
      'cancel',
      'expectedLength',
    ]) {
      Object.defineProperty(source, name, {
        get() {
          order.push(`source.${name}`);
          return name === 'type' ? 'bytes' : undefined;
        },
      });
    }
    new ReadableStream(source, {
      get highWaterMark() {
        order.push('strategy.hwm');
        return {
          valueOf() {
            order.push('hwm.valueOf');
            return 0;
          },
        };
      },
      get size() {
        order.push('strategy.size');
        return undefined;
      },
    });
    const strategy = ['strategy.hwm', 'hwm.valueOf', 'strategy.size'];
    deepStrictEqual(
      order,
      usingTsImpl
        ? [
            ...strategy,
            'source.type',
            'source.autoAllocateChunkSize',
            'source.cancel',
            'source.expectedLength',
            'source.pull',
            'source.start',
          ]
        : [
            'source.type',
            'source.autoAllocateChunkSize',
            'source.start',
            'source.pull',
            'source.cancel',
            'source.expectedLength',
            ...strategy,
          ]
    );
  },
};

// DIVERGENCE: which error wins when two are possible on a byte source
// (the readable suite's conversionErrorPrecedence). The spec converts
// every member of the source before it checks the strategy, and C++
// converts the source before the strategy, so a non-callable pull
// (TypeError) beats an invalid highWaterMark and a size function there;
// TypeScript checks both before it reads pull, so their RangeErrors win.
// A negative autoAllocateChunkSize beats a non-callable pull under
// TypeScript, as in the spec, where autoAllocateChunkSize converts
// first; C++'s int accepts -1 and checks its range later, so pull's
// TypeError wins. Both check a zero autoAllocateChunkSize after the
// highWaterMark (spec), whose error wins: TypeError in C++, RangeError
// in TypeScript, as for any invalid highWaterMark.
export const conversionErrorPrecedence = {
  test() {
    const pullError = {
      name: 'TypeError',
      message:
        "Incorrect type for the 'pull' field on 'UnderlyingSource': the provided value is not of type 'function'.",
    };
    const highWaterMarkError = usingTsImpl
      ? { name: 'RangeError', message: 'Invalid highWaterMark' }
      : {
          name: 'TypeError',
          message:
            'The value cannot be converted because it is negative and this API expects a positive bigint.',
        };
    throws(
      () =>
        new ReadableStream({ type: 'bytes', pull: 1 }, { highWaterMark: -1 }),
      usingTsImpl ? highWaterMarkError : pullError
    );
    throws(
      () => new ReadableStream({ type: 'bytes', pull: 1 }, { size() {} }),
      usingTsImpl
        ? {
            name: 'RangeError',
            message:
              'The strategy for a byte stream cannot have a size function',
          }
        : pullError
    );
    throws(
      () =>
        new ReadableStream({
          type: 'bytes',
          autoAllocateChunkSize: -1,
          pull: 1,
        }),
      usingTsImpl
        ? {
            name: 'TypeError',
            message:
              'autoAllocateChunkSize must be a finite number from 0 to 2^53 - 1',
          }
        : pullError
    );
    throws(
      () =>
        new ReadableStream(
          { type: 'bytes', autoAllocateChunkSize: 0 },
          { highWaterMark: -1 }
        ),
      highWaterMarkError
    );
  },
};

// A byte stream's default high-water mark is 0: desiredSize is 0 inside
// start() and — unlike a value stream's default hwm 1 — NO automatic
// pull follows (parity; the byte half of streams-js-test.js hwmDefault).
export const byteHwmDefaultIsZero = {
  async test() {
    let pulled = 0;
    let seen;
    new ReadableStream({
      type: 'bytes',
      start(c) {
        seen = c.desiredSize;
      },
      pull() {
        pulled++;
      },
    });
    strictEqual(seen, 0);
    await scheduler.wait(10);
    strictEqual(pulled, 0);
  },
};

// DIVERGENCE (readable suite ledger #6 mirrored): a synchronously
// throwing start() escapes the constructor under TypeScript (spec) but
// is captured by C++ — construction succeeds and the stream is errored.
export const syncStartThrow = {
  async test() {
    const err = new Error('start-throw');
    if (usingTsImpl) {
      let caught;
      try {
        new ReadableStream({
          type: 'bytes',
          start() {
            throw err;
          },
        });
      } catch (e) {
        caught = e;
      }
      strictEqual(caught, err);
    } else {
      const rs = new ReadableStream({
        type: 'bytes',
        start() {
          throw err;
        },
      });
      let readErr;
      await rs
        .getReader()
        .read()
        .catch((e) => (readErr = e));
      strictEqual(readErr, err);
    }
  },
};
