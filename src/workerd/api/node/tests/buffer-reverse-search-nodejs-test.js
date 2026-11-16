// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

// Split from buffer-nodejs-test.js so that it can skip ThreadSanitizer (see BUILD.bazel).

import { strictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';

export const thueMorseLastIndexOf = {
  test() {
    // Generate a really long Thue-Morse sequence of 'yolo' and 'swag',
    // "yolo swag swag yolo swag yolo yolo swag" ..., goes on for about 5MB.
    // This is hard to search because it all looks similar, but never repeats.

    // countBits returns the number of bits in the binary representation of n.
    function countBits(n) {
      let count;
      for (count = 0; n > 0; count++) {
        n = n & (n - 1); // remove top bit
      }
      return count;
    }
    const parts = [];
    for (let i = 0; i < 1000000; i++) {
      parts.push(countBits(i) % 2 === 0 ? 'yolo' : 'swag');
    }
    const reallyLong = Buffer.from(parts.join(' '));
    strictEqual(reallyLong.slice(0, 19).toString(), 'yolo swag swag yolo');

    // Expensive reverse searches. Stress test lastIndexOf:
    let pattern = reallyLong.slice(0, 100000); // First 1/50th of the pattern.
    strictEqual(reallyLong.lastIndexOf(pattern), 4751360);
    strictEqual(reallyLong.lastIndexOf(pattern, 4000000), 3932160);
    strictEqual(reallyLong.lastIndexOf(pattern, 3000000), 2949120);
    pattern = reallyLong.slice(100000, 200000); // Second 1/50th.
    strictEqual(reallyLong.lastIndexOf(pattern), 4728480);
    pattern = reallyLong.slice(0, 1000000); // First 1/5th.
    strictEqual(reallyLong.lastIndexOf(pattern), 3932160);
    pattern = reallyLong.slice(0, 2000000); // first 2/5ths.
    strictEqual(reallyLong.lastIndexOf(pattern), 0);
  },
};
