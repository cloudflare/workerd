// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructor format validation: exactly 'deflate', 'gzip', 'deflate-raw'.
// Divergence for non-string (including missing) formats: TypeScript
// ToString-coerces the argument and fails format validation, while the C++
// jsg layer rejects non-strings at the type boundary before validation.

import { ok, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const formatMessage =
  "The compression format must be either 'deflate', 'deflate-raw' or 'gzip'.";

export const validFormatsConstruct = {
  test() {
    for (const format of ['deflate', 'gzip', 'deflate-raw']) {
      ok(new CompressionStream(format));
      ok(new DecompressionStream(format));
    }
  },
};

export const invalidFormatThrows = {
  test() {
    const check = (err) => {
      strictEqual(err.constructor, TypeError);
      strictEqual(err.message, formatMessage);
      return true;
    };
    for (const Ctor of [CompressionStream, DecompressionStream]) {
      throws(() => new Ctor('br'), check);
      throws(() => new Ctor('GZIP'), check); // case-sensitive
    }
  },
};

export const formatToStringCoercedOnce = {
  test() {
    // An object format is ToString-coerced exactly once in both
    // implementations; a valid result constructs normally.
    let called = 0;
    const format = {
      toString() {
        called++;
        return 'gzip';
      },
    };
    ok(new CompressionStream(format));
    strictEqual(called, 1);
  },
};

export const nonStringFormatThrows = {
  test() {
    for (const Ctor of [CompressionStream, DecompressionStream]) {
      // null stringifies to "null" in both implementations and fails
      // format validation.
      throws(
        () => new Ctor(null),
        (err) => {
          strictEqual(err.constructor, TypeError);
          strictEqual(err.message, formatMessage);
          return true;
        }
      );
      // A missing (undefined) argument: TypeScript ToString-coerces it
      // into format validation; the C++ jsg layer rejects undefined at
      // the type boundary.
      throws(
        () => new Ctor(),
        (err) => {
          strictEqual(err.constructor, TypeError);
          strictEqual(
            err.message,
            usingTsImpl
              ? formatMessage
              : `Failed to construct '${Ctor.name}': constructor parameter ` +
                  "1 is not of type 'string'."
          );
          return true;
        }
      );
    }
  },
};
