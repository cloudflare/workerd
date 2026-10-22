// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructor validation. Complements WPT readable-streams/general.any
// and constructor.any, whose C++ expectedFailures here narrow to
// argument-conversion ORDER and validation types.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// DIVERGENCE: a null underlyingSource is accepted by C++, which converts
// it as a WebIDL dictionary (no source), but rejected by TypeScript
// (spec: the argument is typed `object`, which null is not). Every other
// primitive throws TypeError, and a function is accepted and its members
// read, in both (spec).
export const garbageSourceValidation = {
  test() {
    const sourceError = {
      name: 'TypeError',
      message: 'underlyingSource must be an object',
    };
    if (usingTsImpl) {
      throws(() => new ReadableStream(null), sourceError);
    } else {
      new ReadableStream(null);
    }
    for (const source of [42, 'x', true, 1n, Symbol('s')]) {
      throws(
        () => new ReadableStream(source),
        usingTsImpl ? sourceError : TypeError
      );
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

// DIVERGENCE: argument conversion order (the WPT constructor.any seed),
// every member of a default source logged. The spec (TypeScript) converts
// the queuingStrategy FIRST (highWaterMark and its ToNumber, then size),
// then the source; C++ converts the source first, then the strategy in
// the same order. Both read every member of the source: C++ in its own
// order, TypeScript in the spec's lexicographic order (a byte source:
// the readable-byte suite's argumentConversionOrder).
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
          return undefined;
        },
      });
    }
    new ReadableStream(source, {
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
    });
    const strategy = ['strategy.hwm', 'hwm.valueOf', 'strategy.size'];
    deepStrictEqual(
      order,
      usingTsImpl
        ? [
            ...strategy,
            'source.autoAllocateChunkSize',
            'source.cancel',
            'source.expectedLength',
            'source.pull',
            'source.start',
            'source.type',
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

// The strategy argument converts as a WebIDL dictionary in both (spec):
// null means no strategy, a function's members are read, and every other
// primitive throws TypeError (messages differ).
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
    strictEqual(desiredSize(null), 1);
    for (const strategy of [5, 'x', true, 1n, Symbol('s')]) {
      throws(
        () => desiredSize(strategy),
        usingTsImpl
          ? {
              name: 'TypeError',
              message: 'The queuing strategy must be an object.',
            }
          : TypeError
      );
    }
    const strategy = Object.assign(function strategy() {}, {
      highWaterMark: 3,
    });
    strictEqual(desiredSize(strategy), 3);
  },
};

// DIVERGENCE: which error wins when two are possible. Both reject a
// primitive source before reading the strategy, so its TypeError beats a
// throwing strategy getter (spec). An invalid type and a non-callable
// size beat an invalid highWaterMark under TypeScript, as the spec's
// ExtractHighWaterMark range check comes after both; C++ converts
// highWaterMark to an integer before validating anything else, so its
// TypeError wins. Both convert every member of the source before they
// check the type or the highWaterMark (spec), so a non-callable pull
// (TypeError, messages differ) or a throwing pull getter beats an
// invalid type, and a non-callable pull beats an invalid highWaterMark.
export const conversionErrorPrecedence = {
  test() {
    const boom = new Error('boom');
    const pullError = {
      name: 'TypeError',
      message: usingTsImpl
        ? 'underlyingSource.pull must be a function'
        : "Incorrect type for the 'pull' field on 'UnderlyingSource': the provided value is not of type 'function'.",
    };
    throws(
      () =>
        new ReadableStream(42, {
          get highWaterMark() {
            throw boom;
          },
        }),
      usingTsImpl
        ? { name: 'TypeError', message: 'underlyingSource must be an object' }
        : TypeError
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
    throws(() => new ReadableStream({ type: 'potato', pull: 1 }), pullError);
    throws(
      () =>
        new ReadableStream({
          type: 'potato',
          get pull() {
            throw boom;
          },
        }),
      (e) => e === boom
    );
    throws(
      () => new ReadableStream({ pull: 1 }, { highWaterMark: -1 }),
      pullError
    );
  },
};

// DIVERGENCE: autoAllocateChunkSize and expectedLength on a default
// source, which uses neither. Both convert them on every source, as on a
// byte source but with no zero check (spec): expectedLength as a uint64,
// so 'abc' and -1 throw TypeError (messages differ; the readable-byte
// suite's ledger #35), and autoAllocateChunkSize as a number whose
// fraction truncates, so 0, 1.5 and 2 ** 31 - 1 are accepted. C++
// converts autoAllocateChunkSize as an int: it accepts -1, 'abc', NaN and
// Infinity, and throws from 2 ** 31. TypeScript converts it as the spec's
// [EnforceRange] unsigned long long: it throws for those, and accepts up
// to 2 ** 53 - 1.
export const byteMembersOnDefaultSource = {
  test() {
    const outOfRange = {
      name: 'TypeError',
      message:
        'autoAllocateChunkSize must be a finite number from 0 to 2^53 - 1',
    };
    const outOfIntRange = {
      name: 'TypeError',
      message:
        'Value out of range. Must be between -2147483648 and 2147483647 (inclusive).',
    };
    for (const autoAllocateChunkSize of [0, 1.5, 2 ** 31 - 1]) {
      new ReadableStream({ autoAllocateChunkSize });
    }
    for (const autoAllocateChunkSize of [-1, 'abc', NaN, Infinity]) {
      const construct = () => new ReadableStream({ autoAllocateChunkSize });
      if (usingTsImpl) {
        throws(construct, outOfRange);
      } else {
        construct();
      }
    }
    const beyondInt = () =>
      new ReadableStream({ autoAllocateChunkSize: 2 ** 31 });
    if (usingTsImpl) {
      beyondInt();
    } else {
      throws(beyondInt, outOfIntRange);
    }
    throws(
      () => new ReadableStream({ autoAllocateChunkSize: 2 ** 53 }),
      usingTsImpl ? outOfRange : outOfIntRange
    );
    for (const [expectedLength, message] of [
      ['abc', 'The value cannot be converted because it is not an integer.'],
      [
        -1,
        'The value cannot be converted because it is negative and this API expects a positive bigint.',
      ],
    ]) {
      throws(() => new ReadableStream({ expectedLength }), {
        name: 'TypeError',
        message: usingTsImpl
          ? 'expectedLength must be a non-negative number that fits in a uint64'
          : message,
      });
    }
    new ReadableStream({ expectedLength: 1.5 });
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
