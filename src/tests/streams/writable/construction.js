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

import { throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Invalid highWaterMark values are rejected at construction; the error
// type diverges per implementation.
export const highWaterMarkValidated = {
  test() {
    const expected = usingTsImpl ? RangeError : TypeError;
    [-1, -Infinity, NaN, {}, 'foo'].forEach((highWaterMark) => {
      throws(() => new WritableStream(undefined, { highWaterMark }), expected);
    });
  },
};
