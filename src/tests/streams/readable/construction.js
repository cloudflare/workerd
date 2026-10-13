// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructor validation. Complements WPT readable-streams/general.any
// and constructor.any, whose C++ expectedFailures here narrow to
// argument-conversion ORDER and validation types.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// DIVERGENCE (mirror-image!): C++ converts the underlyingSource as a
// WebIDL dictionary: null means no source and every other primitive
// throws TypeError. TypeScript rejects null (spec: the argument is typed
// `object`, which null is not) but accepts every other primitive,
// reading its members through the wrapper prototype. Both accept a
// function and read its members, as the spec does.
export const garbageSourceValidation = {
  test() {
    const primitives = [42, 'x', true, 1n, Symbol('s')];
    if (usingTsImpl) {
      throws(() => new ReadableStream(null), {
        name: 'TypeError',
        message: 'Cannot convert undefined or null to object',
      });
      for (const source of primitives) {
        new ReadableStream(source);
      }
    } else {
      new ReadableStream(null);
      for (const source of primitives) {
        throws(() => new ReadableStream(source), TypeError);
      }
    }
    let started = false;
    new ReadableStream(
      Object.assign(function source() {}, {
        start() {
          started = true;
        },
      })
    );
    strictEqual(started, true);
  },
};

// An invalid underlyingSource.type throws TypeError on both sides, with
// different messages.
export const invalidTypeValidation = {
  test() {
    throws(() => new ReadableStream({ type: 'potato' }), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'Invalid underlying source type: potato'
        : '"potato" is not a valid type of ReadableStream.',
    });
    // undefined is valid (default value stream).
    new ReadableStream({ type: undefined });
  },
};

// DIVERGENCE: argument conversion order (the WPT constructor.any seed).
// The spec converts the queuingStrategy FIRST (highWaterMark and its
// ToNumber, then size), then reads the source's type. TypeScript reads
// the strategy first but size before highWaterMark, and converts
// highWaterMark only after reading the source's type; C++ reads the
// source's type first, then highWaterMark and its ToNumber, then size.
export const argumentConversionOrder = {
  test() {
    const order = [];
    new ReadableStream(
      {
        get type() {
          order.push('source.type');
          return undefined;
        },
      },
      {
        get size() {
          order.push('strategy.size');
          return undefined;
        },
        get highWaterMark() {
          order.push('strategy.hwm');
          return {
            valueOf() {
              order.push('hwm.valueOf');
              return 1;
            },
          };
        },
      }
    );
    strictEqual(
      order.join(','),
      usingTsImpl
        ? 'strategy.size,strategy.hwm,source.type,hwm.valueOf'
        : 'source.type,strategy.hwm,hwm.valueOf,strategy.size'
    );
  },
};

// DIVERGENCE: C++ converts the strategy argument as a WebIDL dictionary:
// null means no strategy and every other primitive throws TypeError.
// TypeScript throws for null (reading its size) and accepts every other
// primitive as no strategy, reading its members through the wrapper
// prototype. Both read a function's members, as the spec does.
export const strategyArgumentShapes = {
  test() {
    const desiredSize = (strategy) => {
      let seen;
      new ReadableStream(
        {
          start(c) {
            seen = c.desiredSize;
          },
        },
        strategy
      );
      return seen;
    };
    const primitives = [5, 'x', true, 1n, Symbol('s')];
    if (usingTsImpl) {
      throws(() => desiredSize(null), {
        name: 'TypeError',
        message: "Cannot read properties of null (reading 'size')",
      });
      for (const strategy of primitives) {
        strictEqual(desiredSize(strategy), 1);
      }
    } else {
      strictEqual(desiredSize(null), 1);
      for (const strategy of primitives) {
        throws(() => desiredSize(strategy), TypeError);
      }
    }
    const strategy = Object.assign(function strategy() {}, {
      highWaterMark: 3,
    });
    strictEqual(desiredSize(strategy), 3);
  },
};

// DIVERGENCE: which error wins when two are possible. C++ converts the
// source before reading the strategy, so a primitive source beats a
// throwing strategy getter, and converts highWaterMark to an integer
// before validating anything else, so an invalid highWaterMark beats an
// invalid type and a non-callable size. TypeScript reads the strategy
// first, so the getter's error beats the primitive source (the spec
// rejects the source first); an invalid type and a non-callable size
// beat an invalid highWaterMark, as the spec's ExtractHighWaterMark
// range check comes after both.
export const conversionErrorPrecedence = {
  test() {
    const boom = new Error('boom');
    throws(
      () =>
        new ReadableStream(42, {
          get highWaterMark() {
            throw boom;
          },
        }),
      usingTsImpl ? (e) => e === boom : TypeError
    );
    throws(
      () => new ReadableStream({ type: 'potato' }, { highWaterMark: -1 }),
      {
        name: 'TypeError',
        message: usingTsImpl
          ? 'Invalid underlying source type: potato'
          : 'The value cannot be converted because it is negative and this API expects a positive bigint.',
      }
    );
    throws(() => new ReadableStream({}, { size: 'x', highWaterMark: NaN }), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'strategy.size must be a function'
        : 'The value cannot be converted because it is not an integer.',
    });
  },
};

// DIVERGENCE (the transform suite's ledger #6/#9 mirrored onto
// ReadableStream): invalid highWaterMark values throw RangeError under
// TypeScript and TypeError under C++, and Infinity — valid per spec —
// is accepted by TypeScript but rejected by the C++ integer conversion.
export const highWaterMarkValidated = {
  test() {
    for (const hwm of [-1, NaN]) {
      throws(() => new ReadableStream({}, { highWaterMark: hwm }), {
        name: usingTsImpl ? 'RangeError' : 'TypeError',
      });
    }
    if (usingTsImpl) {
      new ReadableStream({}, { highWaterMark: Infinity });
    } else {
      throws(() => new ReadableStream({}, { highWaterMark: Infinity }), {
        name: 'TypeError',
        message: 'The value cannot be converted because it is not an integer.',
      });
    }
  },
};

// The default strategy for a value stream is highWaterMark 1: desiredSize
// is 1 inside start() and exactly one pull follows (migrated from
// streams-js-test.js hwmDefault; the byte-stream half lives in the
// readable-byte suite).
export const hwmDefaultIsOne = {
  async test() {
    let pulled = 0;
    let seen;
    new ReadableStream({
      start(c) {
        seen = c.desiredSize;
      },
      pull() {
        pulled++;
      },
    });
    strictEqual(seen, 1);
    await scheduler.wait(10);
    strictEqual(pulled, 1);
  },
};
