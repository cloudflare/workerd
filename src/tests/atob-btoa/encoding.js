// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// btoa(): RFC 4648 base64 of a string whose code units are bytes.

import { strictEqual } from 'node:assert';
import {
  binaryString,
  kNotLatin1,
  referenceEncode,
  throwsInvalidCharacter,
} from 'helpers';

export const encodes = {
  test() {
    const cases = [
      ['', ''],
      ['a', 'YQ=='],
      ['ab', 'YWI='],
      ['abc', 'YWJj'],
      ['abcd', 'YWJjZA=='],
      ['\0', 'AA=='],
      ['\0\0\0', 'AAAA'],
      ['a\0b', 'YQBi'],
      ['\xff', '/w=='],
      ['\xff\xff', '//8='],
      ['\xff\xff\xff', '////'],
      ['\xfb\xff\xbf', '+/+/'],
      ['\x80', 'gA=='],
      ['\xe9', '6Q=='],
      ['Hello, World!', 'SGVsbG8sIFdvcmxkIQ=='],
    ];
    for (const [input, expected] of cases) {
      strictEqual(btoa(input), expected, JSON.stringify(input));
    }
  },
};

export const rejectsCodeUnitsAboveFF = {
  test() {
    const cases = [
      'Ā',
      'abcĀ',
      'Āabc',
      'aŁb',
      '€',
      '￿',
      '\ud800',
      '\udc00',
      '\u{1f600}',
      // A long string whose last code unit alone is out of range.
      binaryString(5000) + 'Ā',
    ];
    for (const input of cases) {
      throwsInvalidCharacter(() => btoa(input), kNotLatin1);
    }
  },
};

export const twoByteStrings = {
  test() {
    // Code units that are all bytes, in a string whose representation is
    // two-byte.
    strictEqual(('abc' + 'Ā').slice(0, -1).length, 3);
    strictEqual(btoa(('abc' + 'Ā').slice(0, -1)), 'YWJj');
    strictEqual(btoa(('\xff\xfe' + 'Ā').slice(0, -1)), '//4=');
    const binary = binaryString(3000);
    strictEqual(btoa((binary + 'Ā').slice(0, -1)), referenceEncode(binary));
  },
};

export const stringShapes = {
  test() {
    // Concatenated, sliced and repeated strings, which are not flat.
    const binary = binaryString(600);
    let cons = '';
    for (const char of binary) cons += char;
    strictEqual(btoa(cons), referenceEncode(binary));
    const sliced = ('Ā' + binary.repeat(4) + 'Ā').slice(1, -1);
    strictEqual(btoa(sliced), referenceEncode(binary.repeat(4)));
  },
};

export const everyLength = {
  test() {
    // Lengths around every buffer size an implementation may special-case.
    const lengths = [];
    for (let length = 0; length <= 300; length++) lengths.push(length);
    for (const near of [765, 768, 1024, 1365, 2048, 4096, 8192, 16384, 65536]) {
      for (let delta = -4; delta <= 4; delta++) lengths.push(near + delta);
    }
    for (const length of lengths) {
      const binary = binaryString(length, length);
      strictEqual(btoa(binary), referenceEncode(binary), `length ${length}`);
    }
  },
};

export const longInput = {
  test() {
    const binary = binaryString(3 * 1024 * 1024);
    strictEqual(btoa(binary) === referenceEncode(binary), true);
  },
};

export const roundTrips = {
  test() {
    let all = '';
    for (let i = 0; i < 256; i++) all += String.fromCharCode(i);
    for (let shift = 0; shift < 3; shift++) {
      const input = all.slice(shift);
      strictEqual(atob(btoa(input)), input);
    }
    const encoded = btoa(all);
    strictEqual(/^[A-Za-z0-9+/]*={0,2}$/.test(encoded), true);
    strictEqual(encoded.length % 4, 0);
    strictEqual(btoa(atob(encoded)), encoded);
  },
};
