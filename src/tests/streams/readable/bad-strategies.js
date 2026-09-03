// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Misbehaving queuing strategies (WPT readable-streams/bad-strategies
// seeds). The C++ side does not validate size() return values at
// enqueue time; TypeScript follows the spec's RangeError paths.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { rejectionOf } from 'helpers';

// size() that errors the stream and then THROWS: under TypeScript the
// enqueue re-throws the size() error (spec); under C++ the enqueue
// swallows it. Both sides leave the stream errored with the error()
// reason (identity).
export const sizeErrorsStreamThenThrows = {
  async test() {
    const err = new Error('errored-from-size');
    let controller;
    const rs = new ReadableStream(
      {
        start(c) {
          controller = c;
        },
      },
      {
        size() {
          controller.error(err);
          throw new Error('size-throw');
        },
        highWaterMark: 5,
      }
    );
    if (usingTsImpl) {
      throws(() => controller.enqueue('a'), { message: 'size-throw' });
    } else {
      controller.enqueue('a');
    }
    strictEqual(await rejectionOf(rs.getReader().read()), err);
  },
};

// size() that errors the stream and then returns Infinity: TypeScript
// rejects the enqueue with RangeError (invalid size); C++ accepts it.
// The stream is errored with the error() reason either way.
export const sizeErrorsStreamThenReturnsInfinity = {
  async test() {
    const err = new Error('errored-from-size');
    let controller;
    const rs = new ReadableStream(
      {
        start(c) {
          controller = c;
        },
      },
      {
        size() {
          controller.error(err);
          return Infinity;
        },
        highWaterMark: 5,
      }
    );
    if (usingTsImpl) {
      throws(() => controller.enqueue('a'), {
        name: 'RangeError',
        message: 'Invalid chunk size',
      });
    } else {
      controller.enqueue('a');
    }
    strictEqual(await rejectionOf(rs.getReader().read()), err);
  },
};

// A non-function strategy.size throws TypeError at construction on both
// sides (different messages).
export const sizeMustBeFunction = {
  test() {
    throws(() => new ReadableStream({}, { size: 42, highWaterMark: 1 }), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'strategy.size must be a function'
        : /Incorrect type for the 'size' field/,
    });
  },
};

// DIVERGENCE (WPT 'invalid strategy.size return value'): NaN or negative
// size() returns make the enqueue throw RangeError under TypeScript and
// leave the stream errored with that RangeError (spec). C++ returns
// normally from the enqueue but ALSO errors the stream (desiredSize
// null). KNOWN DEFECT, not exercisable here: under C++ a subsequent
// read() on a stream errored this way spins the isolate synchronously
// (the reason the WPT bad-strategies cases are expectedFailures rather
// than just wrong-error-type mismatches), so only bounded observables
// are asserted on that side.
export const invalidSizeReturnValue = {
  async test() {
    for (const bad of [NaN, -1]) {
      let controller;
      const rs = new ReadableStream(
        {
          start(c) {
            controller = c;
          },
        },
        {
          size() {
            return bad;
          },
          highWaterMark: 1,
        }
      );
      if (usingTsImpl) {
        throws(() => controller.enqueue('a'), {
          name: 'RangeError',
          message: 'Invalid chunk size',
        });
        const err = await rejectionOf(rs.getReader().read());
        strictEqual(err.name, 'RangeError');
        strictEqual(err.message, 'Invalid chunk size');
      } else {
        controller.enqueue('a');
        strictEqual(controller.desiredSize, null);
      }
    }
  },
};
