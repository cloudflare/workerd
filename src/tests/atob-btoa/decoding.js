// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// atob(): WHATWG Infra's forgiving-base64 decode.

import { strictEqual } from 'node:assert';
import {
  binaryString,
  kInvalidBase64,
  prng,
  referenceDecode,
  referenceEncode,
  throwsInvalidCharacter,
} from 'helpers';

function codes(string) {
  return Array.from(string, (char) => char.charCodeAt(0)).join();
}

export const decodes = {
  test() {
    const cases = [
      ['', ''],
      ['YQ==', 'a'],
      ['YWI=', 'ab'],
      ['YWJj', 'abc'],
      ['YWJjZA==', 'abcd'],
      ['AA==', '\0'],
      ['AAAA', '\0\0\0'],
      ['/w==', '\xff'],
      ['//8=', '\xff\xff'],
      ['////', '\xff\xff\xff'],
      ['+/+/', '\xfb\xff\xbf'],
      ['gICA', '\x80\x80\x80'],
      ['bnVsbA==', 'null'],
    ];
    for (const [input, expected] of cases) {
      strictEqual(codes(atob(input)), codes(expected), input);
    }
  },
};

export const paddingIsOptional = {
  test() {
    strictEqual(atob('YQ'), 'a');
    strictEqual(atob('YWI'), 'ab');
    strictEqual(atob('YWJjZA'), 'abcd');
    strictEqual(atob('YWJjZGU'), 'abcde');
  },
};

export const asciiWhitespaceIsRemoved = {
  test() {
    // Tab, line feed, form feed, carriage return and space, anywhere,
    // padding included, and before the length is taken.
    strictEqual(atob(' Y\tW\nJ\fj\r '), 'abc');
    strictEqual(atob('YQ =\n='), 'a');
    strictEqual(atob(' YQ== '), 'a');
    strictEqual(atob('Y Q'), 'a');
    strictEqual(atob('YQ= ='), 'a');
    strictEqual(atob(' '), '');
    strictEqual(atob('\t\n\f\r '), '');
    strictEqual(atob('YWJj\r\nZGVm\r\n'), 'abcdef');
  },
};

export const discardsTrailingBits = {
  test() {
    // The bits of the last character that no byte takes need not be zero.
    strictEqual(atob('YR=='), 'a');
    strictEqual(atob('YR'), 'a');
    strictEqual(atob('YWJ='), 'ab');
    strictEqual(atob('YWJ'), 'ab');
    strictEqual(codes(atob('//')), '255');
  },
};

export const rejects = {
  test() {
    const cases = [
      // A length, without whitespace and padding, of 1 modulo 4.
      'Y',
      'YWJjZ',
      'Y===',
      'A===',
      // Padding where the length is not a multiple of 4, in the middle, or
      // more than two.
      'YQ=',
      'YQ===',
      'YWI==',
      'YWJj=',
      'YWJj==',
      '=',
      '==',
      '===',
      '====',
      '=YQ=',
      'YQ=a',
      'YQ==a',
      'a=b=',
      'ab=c',
      'abc=d',
      'abcd=',
      'abcd=efgh',
      'YQ==YQ==',
      'YQ==YQ',
      'YQ==  YQ==',
      // Characters outside the alphabet: the URL-safe alphabet, other
      // whitespace, NUL, and anything not ASCII.
      'YW-_',
      'Y-_/',
      'YQ\v==',
      'YQ ==',
      'YQ ==',
      '﻿YQ==',
      'YQ\0==',
      'YQ==\0',
      '\0',
      'YW\0Jj',
      '!',
      'YQ.=',
      'YQ,=',
      'é',
      'YQé=',
      'Ā',
      'YWJjĀ',
      '\ud800',
      '\udc00',
      'YWJj\ud800',
      '\u{1f600}',
      // U+FF21, a fullwidth 'A', whose low byte is 'A'.
      'ＡＡＡＡ',
      // U+0141, U+0142: their low bytes are 'A' and 'B'.
      'ŁłŁł',
    ];
    for (const input of cases) {
      strictEqual(referenceDecode(input), null, JSON.stringify(input));
      throwsInvalidCharacter(() => atob(input), kInvalidBase64);
    }
  },
};

export const twoByteStrings = {
  test() {
    // Valid input in a string whose representation is two-byte.
    const twoByte = ('YWJj' + 'Ā').slice(0, -1);
    strictEqual(atob(twoByte), 'abc');
    const long = (referenceEncode(binaryString(3000)) + 'Ā').slice(0, -1);
    strictEqual(atob(long), binaryString(3000));
  },
};

export const stringShapes = {
  test() {
    // Concatenated, sliced and repeated strings, which are not flat.
    const encoded = referenceEncode(binaryString(600));
    let cons = '';
    for (const char of encoded) cons += char;
    strictEqual(atob(cons), binaryString(600));
    const sliced = ('!!!!' + encoded.repeat(4) + '!!!!').slice(4, -4);
    strictEqual(atob(sliced), binaryString(600).repeat(4));
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
      const encoded = referenceEncode(binary);
      strictEqual(atob(encoded), binary, `length ${length}`);
      strictEqual(
        atob(encoded.replace(/=+$/, '')),
        binary,
        `length ${length}, unpadded`
      );
    }
  },
};

export const longInput = {
  test() {
    const binary = binaryString(3 * 1024 * 1024);
    const encoded = referenceEncode(binary);
    strictEqual(atob(encoded) === binary, true);
    // As MIME wraps it.
    const wrapped = encoded.replace(/.{76}/g, '$&\r\n');
    strictEqual(atob(wrapped) === binary, true);
    // An error at the very end of a long input.
    throwsInvalidCharacter(() => atob(encoded + '!'), kInvalidBase64);
    throwsInvalidCharacter(() => atob(encoded + 'A'), kInvalidBase64);
  },
};

export const resultIsABinaryString = {
  test() {
    // Every byte value comes back as the code unit of that value.
    let all = '';
    for (let i = 0; i < 256; i++) all += String.fromCharCode(i);
    const decoded = atob(referenceEncode(all));
    strictEqual(decoded.length, 256);
    for (let i = 0; i < 256; i++) strictEqual(decoded.charCodeAt(i), i);
  },
};

export const agreesWithTheSpecification = {
  test() {
    // Random strings over the alphabet, padding, whitespace and a few
    // invalid characters, against the algorithm written out in helpers.js.
    const random = prng(0x5eed);
    const kChars = 'ABCDEFGHabcdefgh01234567+/+/====    \t\n\f\r' + '-_!\0\véŁ';
    let valid = 0;
    let invalid = 0;
    for (let i = 0; i < 20000; i++) {
      const length = Math.floor(random() * 12);
      let input = '';
      for (let j = 0; j < length; j++) {
        // Mostly from the first 48, which are valid somewhere.
        const range = random() < 0.9 ? 41 : kChars.length;
        input += kChars[Math.floor(random() * range)];
      }
      const expected = referenceDecode(input);
      if (expected === null) {
        invalid++;
        throwsInvalidCharacter(() => atob(input), kInvalidBase64);
      } else {
        valid++;
        strictEqual(codes(atob(input)), codes(expected), JSON.stringify(input));
      }
    }
    // The generator exercises both outcomes.
    strictEqual(valid > 1000, true);
    strictEqual(invalid > 1000, true);
  },
};
