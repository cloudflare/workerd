// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Byte-stream construction: type/strategy validation and defaults.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// DIVERGENCE (the WPT general.any seed): a size() in the strategy of a
// byte stream is rejected by TypeScript (spec: RangeError) but silently
// accepted — and ignored — by C++.
export const sizeStrategyForBytes = {
  test() {
    if (usingTsImpl) {
      throws(
        () =>
          new ReadableStream(
            { type: 'bytes' },
            {
              size() {
                return 1;
              },
              highWaterMark: 4,
            }
          ),
        {
          name: 'RangeError',
          message: 'The strategy for a byte stream cannot have a size function',
        }
      );
    } else {
      new ReadableStream(
        { type: 'bytes' },
        {
          size() {
            return 1;
          },
          highWaterMark: 4,
        }
      );
    }
  },
};

// autoAllocateChunkSize must be a positive integer: zero, negative, and
// NaN all throw TypeError on both sides (different messages).
export const autoAllocateChunkSizeValidated = {
  test() {
    for (const bad of [0, -1, NaN]) {
      throws(
        () => new ReadableStream({ type: 'bytes', autoAllocateChunkSize: bad }),
        {
          name: 'TypeError',
          message: usingTsImpl
            ? 'autoAllocateChunkSize must be a positive integer'
            : 'The autoAllocateChunkSize option cannot be zero.',
        }
      );
    }
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
