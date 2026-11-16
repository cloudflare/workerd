// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, strictEqual, throws } from 'node:assert';

const blob = new Blob(['abcdef'], { type: 'a/b' });
const text = (b) => b.text();

export const sliceRanges = {
  async test() {
    strictEqual(await text(blob.slice(1.7)), 'bcdef');
    strictEqual(await text(blob.slice(-2)), 'ef');
    strictEqual(await text(blob.slice('2', '4')), 'cd');
    strictEqual(
      await text(
        blob.slice({
          valueOf() {
            return 3;
          },
        })
      ),
      'def'
    );
    strictEqual(await text(blob.slice(4, 2)), '');
    strictEqual(await text(blob.slice(-100, 100)), 'abcdef');
    // Non-finite values and null convert to 0.
    strictEqual(blob.slice(NaN, NaN).size, 0);
    strictEqual(blob.slice(null, null).size, 0);
    strictEqual(blob.slice(1, Infinity).size, 0);
    // An int, as JSG converts one.
    for (const value of [2 ** 40, -(2 ** 40)]) {
      throws(() => blob.slice(value), {
        name: 'TypeError',
        message:
          'Value out of range. Must be between -2147483648 and 2147483647 (inclusive).',
      });
    }
    throws(() => blob.slice(Symbol()), {
      name: 'TypeError',
      message: 'Cannot convert a Symbol value to a number',
    });
  },
};

export const sliceType = {
  test() {
    strictEqual(blob.slice(0, 2, 'C/D').type, 'c/d');
    strictEqual(blob.slice(0, 2).type, '');
    strictEqual(blob.slice(0, 2, 'é').type, '');
  },
};

export const sliceIsANewBlob = {
  async test() {
    const slice = blob.slice();
    strictEqual(slice === blob, false);
    strictEqual(Object.getPrototypeOf(slice), Blob.prototype);
    const file = new File(['xyz'], 'f');
    strictEqual(Object.getPrototypeOf(file.slice(1)), Blob.prototype);
    strictEqual(await text(file.slice(1)), 'yz');
    // Slices of slices.
    strictEqual(await text(blob.slice(1).slice(1).slice(1, -1)), 'de');
    strictEqual(new Blob().slice(1, 2).size, 0);
  },
};

export const sliceConversionOrder = {
  test() {
    const log = [];
    blob.slice(
      {
        valueOf() {
          log.push('start');
          return 0;
        },
      },
      {
        valueOf() {
          log.push('end');
          return 1;
        },
      },
      {
        toString() {
          log.push('type');
          return 'x';
        },
      }
    );
    deepStrictEqual(log, ['start', 'end', 'type']);
  },
};
