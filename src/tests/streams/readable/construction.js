// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructor validation. Complements WPT readable-streams/general.any
// and constructor.any, whose C++ expectedFailures here narrow to
// argument-conversion ORDER and validation types.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// DIVERGENCE (mirror-image!): a null underlyingSource is ACCEPTED by C++
// but rejected by TypeScript (spec: converting null to a dictionary
// throws); a NUMBER underlyingSource is rejected by C++ but accepted by
// TypeScript (primitive wrapper objects convert).
export const garbageSourceValidation = {
  test() {
    if (usingTsImpl) {
      throws(() => new ReadableStream(null), {
        name: 'TypeError',
        message: 'Cannot convert undefined or null to object',
      });
      new ReadableStream(42);
    } else {
      new ReadableStream(null);
      throws(() => new ReadableStream(42), TypeError);
    }
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
// The spec (TypeScript) converts the queuingStrategy FIRST (size before
// highWaterMark), then reads the source's type; C++ reads the source's
// type first, then highWaterMark before size.
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
          return 1;
        },
      }
    );
    strictEqual(
      order.join(','),
      usingTsImpl
        ? 'strategy.size,strategy.hwm,source.type'
        : 'source.type,strategy.hwm,strategy.size'
    );
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
