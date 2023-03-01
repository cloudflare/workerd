
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

import {
  ok,
  fail,
  strictEqual,
  throws,
} from 'node:assert';

import {
  Buffer,
} from 'node:buffer';

import {
  StringDecoder,
} from 'node:string_decoder';

import * as string_decoder from 'node:string_decoder';

if (string_decoder.StringDecoder !== StringDecoder) {
  throw new Error('Incorrect default exports');
}

function getArrayBufferViews(buf) {
  const { buffer, byteOffset, byteLength } = buf;

  const out = [];

  const arrayBufferViews = [
    Int8Array,
    Uint8Array,
    Uint8ClampedArray,
    Int16Array,
    Uint16Array,
    Int32Array,
    Uint32Array,
    Float32Array,
    Float64Array,
    BigInt64Array,
    BigUint64Array,
    DataView,
  ];

  for (const type of arrayBufferViews) {
    const { BYTES_PER_ELEMENT = 1 } = type;
    if (byteLength % BYTES_PER_ELEMENT === 0) {
      out.push(new type(buffer, byteOffset, byteLength / BYTES_PER_ELEMENT));
    }
  }
  return out;
}

// Test verifies that StringDecoder will correctly decode the given input
// buffer with the given encoding to the expected output. It will attempt all
// possible ways to write() the input buffer, see writeSequences(). The
// singleSequence allows for easy debugging of a specific sequence which is
// useful in case of test failures.
function test_(encoding, input, expected, singleSequence) {
  let sequences;
  if (!singleSequence) {
    sequences = writeSequences(input.length);
  } else {
    sequences = [singleSequence];
  }
  const hexNumberRE = /.{2}/g;
  sequences.forEach((sequence) => {
    const decoder = new StringDecoder(encoding);
    let output = '';
    sequence.forEach((write) => {
      output += decoder.write(input.slice(write[0], write[1]));
    });
    output += decoder.end();
    if (output !== expected) {
      const message =
        `Expected "${unicodeEscape(expected)}", ` +
        `but got "${unicodeEscape(output)}"\n` +
        `input: ${input.toString('hex').match(hexNumberRE)}\n` +
        `Write sequence: ${JSON.stringify(sequence)}\n` +
        `Full Decoder State: ${decoder}`;
      fail(message);
    }
  });
}

// writeSequences returns an array of arrays that describes all possible ways a
// buffer of the given length could be split up and passed to sequential write
// calls.
//
// e.G. writeSequences(3) will return: [
//   [ [ 0, 3 ] ],
//   [ [ 0, 2 ], [ 2, 3 ] ],
//   [ [ 0, 1 ], [ 1, 3 ] ],
//   [ [ 0, 1 ], [ 1, 2 ], [ 2, 3 ] ]
// ]
function writeSequences(length, start, sequence) {
  if (start === undefined) {
    start = 0;
    sequence = [];
  } else if (start === length) {
    return [sequence];
  }
  let sequences = [];
  for (let end = length; end > start; end--) {
    const subSequence = sequence.concat([[start, end]]);
    const subSequences = writeSequences(length, end, subSequence, sequences);
    sequences = sequences.concat(subSequences);
  }
  return sequences;
}

// unicodeEscape prints the str contents as unicode escape codes.
function unicodeEscape(str) {
  let r = '';
  for (let i = 0; i < str.length; i++) {
    r += `\\u${str.charCodeAt(i).toString(16)}`;
  }
  return r;
}

export const stringDecoder = {
  test(ctrl, env, ctx) {

    // Test default encoding
    let decoder = new StringDecoder();
    strictEqual(decoder.encoding, 'utf8');

    // Should work without 'new' keyword
    const decoder2 = {};
    StringDecoder.call(decoder2);
    strictEqual(decoder2.encoding, 'utf8');

    // UTF-8
    test_('utf-8', Buffer.from('$', 'utf-8'), '$');
    test_('utf-8', Buffer.from('¢', 'utf-8'), '¢');
    test_('utf-8', Buffer.from('€', 'utf-8'), '€');
    test_('utf-8', Buffer.from('𤭢', 'utf-8'), '𤭢');
    // A mixed ascii and non-ascii string
    // Test stolen from deps/v8/test/cctest/test-strings.cc
    // U+02E4 -> CB A4
    // U+0064 -> 64
    // U+12E4 -> E1 8B A4
    // U+0030 -> 30
    // U+3045 -> E3 81 85

    test_(
      'utf-8',
      Buffer.from([0xCB, 0xA4, 0x64, 0xE1, 0x8B, 0xA4, 0x30, 0xE3, 0x81, 0x85]),
      '\u02e4\u0064\u12e4\u0030\u3045'
    );

    // Some invalid input, known to have caused trouble with chunking
    // in https://github.com/nodejs/node/pull/7310#issuecomment-226445923
    // 00: |00000000 ASCII
    // 41: |01000001 ASCII
    // B8: 10|111000 continuation
    // CC: 110|01100 two-byte head
    // E2: 1110|0010 three-byte head
    // F0: 11110|000 four-byte head
    // F1: 11110|001'another four-byte head
    // FB: 111110|11 "five-byte head", not UTF-8
    test_('utf-8', Buffer.from('C9B5A941', 'hex'), '\u0275\ufffdA');
    test_('utf-8', Buffer.from('E2', 'hex'), '\ufffd');
    test_('utf-8', Buffer.from('E241', 'hex'), '\ufffdA');
    test_('utf-8', Buffer.from('CCCCB8', 'hex'), '\ufffd\u0338');
    test_('utf-8', Buffer.from('F0B841', 'hex'), '\ufffdA');
    test_('utf-8', Buffer.from('F1CCB8', 'hex'), '\ufffd\u0338');
    test_('utf-8', Buffer.from('F0FB00', 'hex'), '\ufffd\ufffd\0');
    test_('utf-8', Buffer.from('CCE2B8B8', 'hex'), '\ufffd\u2e38');
    test_('utf-8', Buffer.from('E2B8CCB8', 'hex'), '\ufffd\u0338');
    test_('utf-8', Buffer.from('E2FBCC01', 'hex'), '\ufffd\ufffd\ufffd\u0001');
    test_('utf-8', Buffer.from('CCB8CDB9', 'hex'), '\u0338\u0379');
    // CESU-8 of U+1D40D

    // V8 has changed their invalid UTF-8 handling, see
    // https://chromium-review.googlesource.com/c/v8/v8/+/671020 for more info.
    test_('utf-8', Buffer.from('EDA0B5EDB08D', 'hex'),
          '\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd');

    // UCS-2
    test_('ucs2', Buffer.from('ababc', 'ucs2'), 'ababc');

    // UTF-16LE
    test_('utf16le', Buffer.from('3DD84DDC', 'hex'), '\ud83d\udc4d'); // thumbs up

    // Additional UTF-8 tests
    decoder = new StringDecoder('utf8');
    strictEqual(decoder.write(Buffer.from('E1', 'hex')), '');

    // A quick test for lastChar, lastNeed & lastTotal which are undocumented.
    ok(decoder.lastChar.equals(new Uint8Array([0xe1, 0, 0, 0])));
    strictEqual(decoder.lastNeed, 2);
    strictEqual(decoder.lastTotal, 3);

    strictEqual(decoder.end(), '\ufffd');

    // ArrayBufferView tests
    const arrayBufferViewStr = 'String for ArrayBufferView tests\n';
    const inputBuffer = Buffer.from(arrayBufferViewStr.repeat(8), 'utf8');
    for (const expectView of getArrayBufferViews(inputBuffer)) {
      strictEqual(
        decoder.write(expectView),
        inputBuffer.toString('utf8')
      );
      strictEqual(decoder.end(), '');
    }

    decoder = new StringDecoder('utf8');
    strictEqual(decoder.write(Buffer.from('E18B', 'hex')), '');
    strictEqual(decoder.end(), '\ufffd');

    decoder = new StringDecoder('utf8');
    strictEqual(decoder.write(Buffer.from('\ufffd')), '\ufffd');
    strictEqual(decoder.end(), '');

    decoder = new StringDecoder('utf8');
    strictEqual(decoder.write(Buffer.from('\ufffd\ufffd\ufffd')),
                '\ufffd\ufffd\ufffd');
    strictEqual(decoder.end(), '');

    decoder = new StringDecoder('utf8');
    strictEqual(decoder.write(Buffer.from('EFBFBDE2', 'hex')), '\ufffd');
    strictEqual(decoder.end(), '\ufffd');

    decoder = new StringDecoder('utf8');
    strictEqual(decoder.write(Buffer.from('F1', 'hex')), '');
    strictEqual(decoder.write(Buffer.from('41F2', 'hex')), '\ufffdA');
    strictEqual(decoder.end(), '\ufffd');

    // Additional utf8Text test
    decoder = new StringDecoder('utf8');
    strictEqual(decoder.text(Buffer.from([0x41]), 2), '');

    // Additional UTF-16LE surrogate pair tests
    decoder = new StringDecoder('utf16le');
    strictEqual(decoder.write(Buffer.from('3DD8', 'hex')), '');
    strictEqual(decoder.write(Buffer.from('4D', 'hex')), '');
    strictEqual(decoder.write(Buffer.from('DC', 'hex')), '\ud83d\udc4d');
    strictEqual(decoder.end(), '');

    decoder = new StringDecoder('utf16le');
    strictEqual(decoder.write(Buffer.from('3DD8', 'hex')), '');
    strictEqual(decoder.end(), '\ud83d');

    decoder = new StringDecoder('utf16le');
    strictEqual(decoder.write(Buffer.from('3DD8', 'hex')), '');
    strictEqual(decoder.write(Buffer.from('4D', 'hex')), '');
    strictEqual(decoder.end(), '\ud83d');

    decoder = new StringDecoder('utf16le');
    strictEqual(decoder.write(Buffer.from('3DD84D', 'hex')), '\ud83d');
    strictEqual(decoder.end(), '');

    // Regression test for https://github.com/nodejs/node/issues/22358
    // (unaligned UTF-16 access).
    decoder = new StringDecoder('utf16le');
    strictEqual(decoder.write(Buffer.alloc(1)), '');
    strictEqual(decoder.write(Buffer.alloc(20)), '\0'.repeat(10));
    strictEqual(decoder.write(Buffer.alloc(48)), '\0'.repeat(24));
    strictEqual(decoder.end(), '');

    // Regression tests for https://github.com/nodejs/node/issues/22626
    // (not enough replacement chars when having seen more than one byte of an
    // incomplete multibyte characters).
    decoder = new StringDecoder('utf8');
    strictEqual(decoder.write(Buffer.from('f69b', 'hex')), '');
    strictEqual(decoder.write(Buffer.from('d1', 'hex')), '\ufffd\ufffd');
    strictEqual(decoder.end(), '\ufffd');
    strictEqual(decoder.write(Buffer.from('f4', 'hex')), '');
    strictEqual(decoder.write(Buffer.from('bde5', 'hex')), '\ufffd\ufffd');
    strictEqual(decoder.end(), '\ufffd');

    throws(
      () => new StringDecoder(1),
      {
        code: 'ERR_UNKNOWN_ENCODING',
        name: 'TypeError',
        message: 'Unknown encoding: 1'
      }
    );

    throws(
      () => new StringDecoder('test'),
      {
        code: 'ERR_UNKNOWN_ENCODING',
        name: 'TypeError',
        message: 'Unknown encoding: test'
      }
    );

    throws(
      () => new StringDecoder('utf8').write(null),
      {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      }
    );

    throws(
      () => new StringDecoder('utf8').__proto__.write(Buffer.from('abc')),
      {
        code: 'ERR_INVALID_THIS',
      }
    );
  }
};

export const stringDecoderEnd = {
  test(ctrl, env, ctx) {
    const encodings = ['base64', 'base64url', 'hex', 'utf8', 'utf16le', 'ucs2'];
    const bufs = [ '☃💩', 'asdf' ].map((b) => Buffer.from(b));

    // Also test just arbitrary bytes from 0-15.
    for (let i = 1; i <= 16; i++) {
      const bytes = '.'.repeat(i - 1).split('.').map((_, j) => j + 0x78);
      bufs.push(Buffer.from(bytes));
    }

    encodings.forEach(testEncoding);

    testEnd('utf8', Buffer.of(0xE2), Buffer.of(0x61), '\uFFFDa');
    testEnd('utf8', Buffer.of(0xE2), Buffer.of(0x82), '\uFFFD\uFFFD');
    testEnd('utf8', Buffer.of(0xE2), Buffer.of(0xE2), '\uFFFD\uFFFD');
    testEnd('utf8', Buffer.of(0xE2, 0x82), Buffer.of(0x61), '\uFFFDa');
    testEnd('utf8', Buffer.of(0xE2, 0x82), Buffer.of(0xAC), '\uFFFD\uFFFD');
    testEnd('utf8', Buffer.of(0xE2, 0x82), Buffer.of(0xE2), '\uFFFD\uFFFD');
    testEnd('utf8', Buffer.of(0xE2, 0x82, 0xAC), Buffer.of(0x61), '€a');

    testEnd('utf16le', Buffer.of(0x3D), Buffer.of(0x61, 0x00), 'a');
    testEnd('utf16le', Buffer.of(0x3D), Buffer.of(0xD8, 0x4D, 0xDC), '\u4DD8');
    testEnd('utf16le', Buffer.of(0x3D, 0xD8), Buffer.of(), '\uD83D');
    testEnd('utf16le', Buffer.of(0x3D, 0xD8), Buffer.of(0x61, 0x00), '\uD83Da');
    testEnd(
      'utf16le',
      Buffer.of(0x3D, 0xD8),
      Buffer.of(0x4D, 0xDC),
      '\uD83D\uDC4D'
    );
    testEnd('utf16le', Buffer.of(0x3D, 0xD8, 0x4D), Buffer.of(), '\uD83D');
    testEnd(
      'utf16le',
      Buffer.of(0x3D, 0xD8, 0x4D),
      Buffer.of(0x61, 0x00),
      '\uD83Da'
    );
    testEnd('utf16le', Buffer.of(0x3D, 0xD8, 0x4D), Buffer.of(0xDC), '\uD83D');
    testEnd(
      'utf16le',
      Buffer.of(0x3D, 0xD8, 0x4D, 0xDC),
      Buffer.of(0x61, 0x00),
      '👍a'
    );

    testEnd('base64', Buffer.of(0x61), Buffer.of(), 'YQ==');
    testEnd('base64', Buffer.of(0x61), Buffer.of(0x61), 'YQ==YQ==');
    testEnd('base64', Buffer.of(0x61, 0x61), Buffer.of(), 'YWE=');
    testEnd('base64', Buffer.of(0x61, 0x61), Buffer.of(0x61), 'YWE=YQ==');
    testEnd('base64', Buffer.of(0x61, 0x61, 0x61), Buffer.of(), 'YWFh');
    testEnd('base64', Buffer.of(0x61, 0x61, 0x61), Buffer.of(0x61), 'YWFhYQ==');

    testEnd('base64url', Buffer.of(0x61), Buffer.of(), 'YQ');
    testEnd('base64url', Buffer.of(0x61), Buffer.of(0x61), 'YQYQ');
    testEnd('base64url', Buffer.of(0x61, 0x61), Buffer.of(), 'YWE');
    testEnd('base64url', Buffer.of(0x61, 0x61), Buffer.of(0x61), 'YWEYQ');
    testEnd('base64url', Buffer.of(0x61, 0x61, 0x61), Buffer.of(), 'YWFh');
    testEnd('base64url', Buffer.of(0x61, 0x61, 0x61), Buffer.of(0x61), 'YWFhYQ');

    function testEncoding(encoding) {
      bufs.forEach((buf) => {
        testBuf(encoding, buf);
      });
    }

    function testBuf(encoding, buf) {
      // Write one byte at a time.
      let s = new StringDecoder(encoding);
      let res1 = '';
      for (let i = 0; i < buf.length; i++) {
        res1 += s.write(buf.slice(i, i + 1));
      }
      res1 += s.end();

      // Write the whole buffer at once.
      let res2 = '';
      s = new StringDecoder(encoding);
      res2 += s.write(buf);
      res2 += s.end();

      // .toString() on the buffer
      const res3 = buf.toString(encoding);

      // One byte at a time should match toString
      strictEqual(res1, res3);
      // All bytes at once should match toString
      strictEqual(res2, res3);
    }

    function testEnd(encoding, incomplete, next, expected) {
      let res = '';
      const s = new StringDecoder(encoding);
      res += s.write(incomplete);
      res += s.end();
      res += s.write(next);
      res += s.end();

      strictEqual(res, expected);
    }
  }
};

export const stringDecoderFuzz = {
  test(ctrl, env, ctx) {
    function rand(max) {
      return Math.floor(Math.random() * max);
    }

    function randBuf(maxLen) {
      const buf = Buffer.allocUnsafe(rand(maxLen));
      for (let i = 0; i < buf.length; i++)
        buf[i] = rand(256);
      return buf;
    }

    const encodings = [
      'utf16le', 'utf8', 'ascii', 'hex', 'base64', 'latin1', 'base64url',
    ];

    function runSingleFuzzTest() {
      const enc = encodings[rand(encodings.length)];
      const sd = new StringDecoder(enc);
      const bufs = [];
      const strings = [];

      const N = rand(10);
      for (let i = 0; i < N; ++i) {
        const buf = randBuf(50);
        bufs.push(buf);
        strings.push(sd.write(buf));
      }
      strings.push(sd.end());

      strictEqual(strings.join(''), Buffer.concat(bufs).toString(enc),
                  `Mismatch:\n${strings}\n` +
                  bufs.map((buf) => buf.toString('hex')) +
                  `\nfor encoding ${enc}`);
    }

    const start = Date.now();
    while (Date.now() - start < 100)
      runSingleFuzzTest();
  }
};

export const stringDecoderHacking = {
  test(ctrl, env, ctx) {
    throws(() => {
      const sd = new StringDecoder();
      const sym = Object.getOwnPropertySymbols(sd)[0];
      sd[sym] = "not a buffer";
      sd.write(Buffer.from("this shouldn't crash"));
    }, {
      name: 'TypeError'
    });

    throws(() => {
      const sd = new StringDecoder();
      const sym = Object.getOwnPropertySymbols(sd)[0];
      sd[sym] = Buffer.alloc(1);
      sd.write(Buffer.from("this shouldn't crash"));
    }, {
      message: 'Invalid StringDecoder'
    });

    throws(() => {
      const sd = new StringDecoder();
      const sym = Object.getOwnPropertySymbols(sd)[0];
      sd[sym] = Buffer.alloc(9);
      sd.write(Buffer.from("this shouldn't crash"));
    }, {
      message: 'Invalid StringDecoder'
    });

    throws(() => {
      const sd = new StringDecoder();
      const sym = Object.getOwnPropertySymbols(sd)[0];
      sd[sym][5] = 100;
      sd.write(Buffer.from("this shouldn't crash"));
    }, {
      message: 'Buffered bytes cannot exceed 4'
    });

    throws(() => {
      const sd = new StringDecoder();
      const sym = Object.getOwnPropertySymbols(sd)[0];
      sd[sym][4] = 100;
      sd.write(Buffer.from("this shouldn't crash"));
    }, {
      message: 'Missing bytes cannot exceed 4'
    });

    throws(() => {
      const sd = new StringDecoder();
      const sym = Object.getOwnPropertySymbols(sd)[0];
      sd[sym][6] = 100;
      sd.write(Buffer.from("this shouldn't crash"));
    }, {
      message: 'Invalid StringDecoder state'
    });

    throws(() => {
      const sd = new StringDecoder();
      const sym = Object.getOwnPropertySymbols(sd)[0];
      sd[sym][4] = 3;
      sd[sym][5] = 2;
      sd.write(Buffer.from("this shouldn't crash"));
    }, {
      message: 'Invalid StringDecoder state'
    });


    {
      // fuzz a bit with random values
      const messages = [
        "Invalid StringDecoder state",
        "Missing bytes cannot exceed 4",
        "Buffered bytes cannot exceed 4",
      ];
      for (let n = 0; n < 255; n++) {
        try {
          const sd = new StringDecoder();
          const sym = Object.getOwnPropertySymbols(sd)[0];
          crypto.getRandomValues(sd[sym]);
          sd.write(Buffer.from("this shouldn't crash"));
        } catch (err) {
          if (!messages.includes(err.message)) {
            throw err;
          }
        }
      }
    }
  }
};
