// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Regression test for a vulnerability where Buffer.toString('base64') on very
// large buffers caused a heap-buffer-overflow. The V8 string creation APIs take
// `int` for the length parameter, but js.str() was passing size_t values that
// could overflow int. With base64 output exceeding ~2.1GB (from input > ~1.6GB),
// the truncated length appeared negative, causing V8 to fall back to strlen()
// and read past the end of the buffer.
//
// The fix adds a check against v8::String::kMaxLength in js.str() before the
// implicit narrowing to int. This test verifies that:
// 1. Normal-sized base64 encoding still works correctly.
// 2. Buffers whose base64 output exceeds kMaxLength throw a RangeError
//    instead of crashing.

import { Buffer } from 'node:buffer';
import { strictEqual, throws } from 'node:assert';

export const base64SmallBuffer = {
  test() {
    // Sanity check: base64 encoding works correctly at normal sizes.
    const buf = Buffer.from('Hello, World!');
    strictEqual(buf.toString('base64'), 'SGVsbG8sIFdvcmxkIQ==');
  },
};

export const base64urlSmallBuffer = {
  test() {
    // Same sanity check for base64url encoding.
    const buf = Buffer.from('Hello, World!');
    strictEqual(buf.toString('base64url'), 'SGVsbG8sIFdvcmxkIQ');
  },
};

export const base64LargeBufferThrowsRangeError = {
  test() {
    // v8::String::kMaxLength is (1 << 29) - 24 = 536,870,888 on 64-bit.
    // Base64 expands by 4/3, so a buffer of 403,000,000 bytes produces
    // base64 output of ~537,333,336 bytes, just over the limit.
    // This must throw a RangeError, not crash.
    const size = 403_000_000;
    const buf = Buffer.alloc(size);
    strictEqual(buf.length, size, 'Buffer allocation must have succeeded');
    throws(() => buf.toString('base64'), {
      name: 'RangeError',
      message: /String is too long for a V8 string/,
    });
  },
};

export const base64urlLargeBufferThrowsRangeError = {
  test() {
    // Same test for base64url encoding.
    const size = 403_000_000;
    const buf = Buffer.alloc(size);
    strictEqual(buf.length, size, 'Buffer allocation must have succeeded');
    throws(() => buf.toString('base64url'), {
      name: 'RangeError',
      message: /String is too long for a V8 string/,
    });
  },
};
