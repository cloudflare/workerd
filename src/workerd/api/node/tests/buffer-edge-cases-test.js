// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Edge cases of `node-internal:buffer`, which has both a C++ and a TypeScript
// implementation (selected by the NODEJS_BUFFER_TS autogate). The default test
// variant exercises the C++ implementation and the @all-autogates variant the
// TypeScript one, so every expectation here holds for both.

import { Buffer, transcode, isUtf8, isAscii } from 'node:buffer';
import { StringDecoder } from 'node:string_decoder';
import { deepStrictEqual, strictEqual, throws } from 'node:assert';

function hex(view) {
  return Buffer.from(view.buffer, view.byteOffset, view.byteLength).toString(
    'hex'
  );
}

function codeUnits(string) {
  const units = [];
  for (let i = 0; i < string.length; i++) units.push(string.charCodeAt(i));
  return units;
}

export const base64Decoding = {
  test() {
    // [input, Buffer.from(input, 'base64'), buf.write(input, 'base64') into 8 bytes]
    // Buffer.from uses simdutf's forgiving decoder; write uses nbytes.
    const cases = [
      ['SGVs bG8=\n', '48656c6c6f', '48656c6c6f'],
      ['SGVsbG8', '48656c6c6f', '48656c6c6f'],
      ['a+b/c-d_', '6be6ff73e77f', '6be6ff73e77f'],
      ['QUJD\0REVG', '414243444546', '414243444546'],
      ['\u0100QUJD', '414243', '414243'],
      ['\ud800QUJD', '414243', '414243'],
      ['QUJD\ud83d\ude00RA', '414243', '41424344'],
      ['QQ==QQ==', '41', '41'],
      ['QU=JD', '41', '41'],
      ['Q', '', ''],
      ['=QUJD', '', ''],
      ['!!!!', '', ''],
    ];
    for (const [input, decoded, written] of cases) {
      strictEqual(hex(Buffer.from(input, 'base64')), decoded, input);
      strictEqual(hex(Buffer.from(input, 'base64url')), decoded, input);

      const dest = Buffer.alloc(8, 0xee);
      strictEqual(dest.write(input, 'base64'), written.length / 2, input);
      strictEqual(
        hex(dest),
        written + 'ee'.repeat(8 - written.length / 2),
        input
      );

      // Decoding stops when the destination is full.
      const small = Buffer.alloc(2, 0xee);
      const expected = written.slice(0, 2);
      strictEqual(small.write(input, 1, 'base64'), expected.length / 2, input);
      strictEqual(hex(small), 'ee' + (expected || 'ee'), input);
    }
  },
};

export const hexDecoding = {
  test() {
    // [input, Buffer.from(input, 'hex'), buf.fill(input, 'hex') on 5 bytes or undefined if it throws]
    const cases = [
      ['abz1', 'ab', undefined],
      ['abc', 'ab', undefined],
      ['zz', '', undefined],
      ['ABcd', 'abcd', 'abcdabcdab'],
      ['0\u0100', '', undefined],
      ['a\ud800b', '', undefined],
      ['', '', '0000000000'],
    ];
    for (const [input, decoded, filled] of cases) {
      strictEqual(hex(Buffer.from(input, 'hex')), decoded, input);

      const dest = Buffer.alloc(4, 0xee);
      strictEqual(dest.write(input, 'hex'), decoded.length / 2, input);
      strictEqual(
        hex(dest),
        decoded + 'ee'.repeat(4 - decoded.length / 2),
        input
      );

      const target = Buffer.alloc(5, 0xee);
      if (filled === undefined) {
        throws(() => target.fill(input, 'hex'), {
          name: 'TypeError',
          message: 'The text is not valid hex',
        });
      } else {
        strictEqual(hex(target.fill(input, 'hex')), filled, input);
      }
    }
  },
};

export const loneSurrogates = {
  test() {
    // [input, { encoding: encoded }, utf8 byteLength]
    const cases = [
      [
        '\ud800',
        { utf8: 'efbfbd', latin1: '00', ascii: '00', utf16le: '00d8' },
        3,
      ],
      [
        'a\udc00b',
        {
          utf8: '61efbfbd62',
          latin1: '610062',
          ascii: '610062',
          utf16le: '610000dc6200',
        },
        5,
      ],
      [
        '\ud83d\ude00',
        {
          utf8: 'f09f9880',
          latin1: '3d00',
          ascii: '3d00',
          utf16le: '3dd800de',
        },
        4,
      ],
      [
        '\u0100\u00ff\u0080x',
        {
          utf8: 'c480c3bfc28078',
          latin1: '00ff8078',
          ascii: '00ff8078',
          utf16le: '0001ff0080007800',
        },
        7,
      ],
    ];
    for (const [input, encoded, byteLength] of cases) {
      for (const [encoding, expected] of Object.entries(encoded)) {
        strictEqual(hex(Buffer.from(input, encoding)), expected, encoding);
      }
      strictEqual(Buffer.byteLength(input), byteLength);
    }

    // [bytes, { encoding: code units of toString(encoding) }]
    const decodeCases = [
      [
        '00d8',
        {
          utf8: [0, 0xfffd],
          latin1: [0, 0xd8],
          ascii: [0, 0x58],
          utf16le: [0xd800],
        },
      ],
      [
        '3dd800de',
        {
          utf8: [0x3d, 0xfffd, 0, 0xfffd],
          latin1: [0x3d, 0xd8, 0, 0xde],
          ascii: [0x3d, 0x58, 0, 0x5e],
          utf16le: [0xd83d, 0xde00],
        },
      ],
      ['ff', { utf8: [0xfffd], latin1: [0xff], ascii: [0x7f], utf16le: [] }],
    ];
    for (const [bytes, decoded] of decodeCases) {
      const buffer = Buffer.from(bytes, 'hex');
      for (const [encoding, expected] of Object.entries(decoded)) {
        deepStrictEqual(codeUnits(buffer.toString(encoding)), expected);
      }
    }
  },
};

export const partialWrites = {
  test() {
    // A multi-byte character that does not fit is not written.
    strictEqual(Buffer.alloc(2).write('\u20ac'), 0);
    strictEqual(Buffer.alloc(3).write('\u20ac'), 3);

    // UTF-16 writes only whole code units.
    const dest = Buffer.alloc(3, 0xee);
    strictEqual(dest.write('abc', 'utf16le'), 2);
    strictEqual(hex(dest), '6100ee');
  },
};

export const utf16IndexOf = {
  test() {
    const haystack = Buffer.from('abcabc', 'utf16le');
    const needle = Buffer.from([0x62, 0x00, 0x63]); // Odd length: 'b' plus half of 'c'.
    // [byteOffset, indexOf('b'), lastIndexOf('b'), indexOf(needle)]
    const cases = [
      [0, 2, -1, 2],
      [1, 2, -1, 2],
      [2, 2, 2, 2],
      [3, 2, 2, 2],
      [4, 8, 2, 8],
      [5, 8, 2, 8],
      [-1, -1, 8, -1],
      [-3, 8, 8, 8],
      [-4, 8, 8, 8],
      [11, -1, 8, -1],
      [12, -1, 8, -1],
      [100, -1, 8, -1],
      [-100, 2, -1, 2],
    ];
    for (const [offset, first, last, bufferFirst] of cases) {
      strictEqual(haystack.indexOf('b', offset, 'utf16le'), first, `${offset}`);
      strictEqual(
        haystack.lastIndexOf('b', offset, 'utf16le'),
        last,
        `${offset}`
      );
      strictEqual(
        haystack.indexOf(needle, offset, 'utf16le'),
        bufferFirst,
        `${offset}`
      );
    }

    // A view with an odd byte offset into its buffer.
    const unaligned = Buffer.concat([Buffer.from([0xff]), haystack]).subarray(
      1
    );
    strictEqual(unaligned.indexOf('ca', 0, 'utf16le'), 4);
    strictEqual(unaligned.lastIndexOf('ab', undefined, 'utf16le'), 6);

    // An odd-length haystack ignores its last byte.
    const oddLength = Buffer.concat([haystack, Buffer.from([0x61])]);
    strictEqual(oddLength.lastIndexOf('a', undefined, 'utf16le'), 6);

    strictEqual(haystack.indexOf('', 100), 12);
    strictEqual(haystack.lastIndexOf('', 100), 12);
    strictEqual(haystack.indexOf('', -100), 0);
    strictEqual(haystack.lastIndexOf('', -100), 0);
    strictEqual(haystack.indexOf(Buffer.alloc(0), 5), 5);
  },
};

// Simple reference search to compare the string-search strategies against.
function naiveIndexOf(haystack, needle, from) {
  for (let i = Math.max(from, 0); i + needle.length <= haystack.length; i++) {
    let j = 0;
    while (j < needle.length && haystack[i + j] === needle[j]) j++;
    if (j === needle.length) return i;
  }
  return -1;
}

function naiveLastIndexOf(haystack, needle, from) {
  for (let i = Math.min(from, haystack.length - needle.length); i >= 0; i--) {
    let j = 0;
    while (j < needle.length && haystack[i + j] === needle[j]) j++;
    if (j === needle.length) return i;
  }
  return -1;
}

export const stringSearchStrategies = {
  test() {
    // Patterns of 8 or more characters start with a linear search and move
    // to Boyer-Moore-Horspool and then Boyer-Moore as they do badly. Highly
    // repetitive text makes them do badly; patterns longer than 250
    // characters exercise the partial Boyer-Moore tables.
    let seed = 1;
    function random(n) {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      return seed % n;
    }
    const alphabets = ['ab', 'abc', 'a\u0100', 'abcdefghijklmnop'];
    for (const alphabet of alphabets) {
      for (const patternLength of [1, 2, 7, 8, 9, 20, 249, 250, 251, 300]) {
        let text = '';
        for (let i = 0; i < 3000; i++) {
          text += alphabet[random(alphabet.length)];
        }
        const start = random(text.length - patternLength);
        const pattern = text.slice(start, start + patternLength);
        // A near miss: a pattern that is not in the text.
        const missing = pattern.slice(0, -1) + '\u00ff';

        for (const encoding of ['latin1', 'utf16le']) {
          const haystack = Buffer.from(text, encoding);
          const width = encoding === 'utf16le' ? 2 : 1;
          const units = encoding === 'utf16le' ? codeUnits(text) : haystack;
          for (const needleString of [pattern, missing]) {
            const needle = Buffer.from(needleString, encoding);
            const needleUnits =
              encoding === 'utf16le' ? codeUnits(needleString) : needle;
            for (const from of [0, 1, 1500, 2999]) {
              const context = `${alphabet} ${patternLength} ${encoding} ${from}`;
              const first = naiveIndexOf(units, needleUnits, from);
              strictEqual(
                haystack.indexOf(needleString, from * width, encoding),
                first === -1 ? -1 : first * width,
                context
              );
              strictEqual(
                haystack.indexOf(needle, from * width, encoding),
                first === -1 ? -1 : first * width,
                context
              );
              const last = naiveLastIndexOf(units, needleUnits, from);
              strictEqual(
                haystack.lastIndexOf(needleString, from * width, encoding),
                last === -1 ? -1 : last * width,
                context
              );
            }
          }
        }
      }
    }
  },
};

export const compareAndConcat = {
  test() {
    const a = Buffer.from('abcdef');
    const b = Buffer.from('abcxyz');
    strictEqual(a.compare(b), -1);
    strictEqual(a.compare(b, 0, 3, 0, 3), 0);
    strictEqual(a.compare(b, 0, 2, 0, 3), 1);
    strictEqual(a.compare(b, 0, 3, 0, 2), -1);
    strictEqual(a.compare(b, 3, 6, 3, 6), -1);
    strictEqual(Buffer.compare(a, a.subarray(0, 3)), 1);
    // Bytes compare as unsigned.
    strictEqual(Buffer.compare(Buffer.from([0x80]), Buffer.from([0x7f])), 1);

    const parts = [Buffer.from('ab'), Buffer.from('cd')];
    strictEqual(hex(Buffer.concat(parts, 3)), '616263');
    strictEqual(hex(Buffer.concat(parts, 6)), '616263640000');
  },
};

export const fill = {
  test() {
    strictEqual(
      hex(Buffer.alloc(7).fill(Buffer.from('abc'))),
      '61626361626361'
    );
    strictEqual(
      hex(Buffer.alloc(7).fill('\u0100', 'latin1')),
      '00000000000000'
    );
    strictEqual(hex(Buffer.alloc(7).fill('ab', 'utf16le')), '61006200610062');
    strictEqual(hex(Buffer.alloc(7).fill('QUI=', 'base64')), '41424142414241');
  },
};

export const swapUnaligned = {
  test() {
    const buffer = Buffer.from('00010203040506070809', 'hex').subarray(1, 9);
    buffer.swap16();
    strictEqual(hex(buffer), '0201040306050807');
    buffer.swap32();
    strictEqual(hex(buffer), '0304010207080506');
    buffer.swap64();
    strictEqual(hex(buffer), '0605080702010403');
  },
};

export const toStringEncodings = {
  test() {
    const bytes = Buffer.from([0xfb, 0xff, 0xfe, 0x00, 0x01]);
    strictEqual(bytes.toString('base64'), '+//+AAE=');
    strictEqual(bytes.toString('base64url'), '-__-AAE');
    // Views with an odd byte offset.
    strictEqual(bytes.subarray(1).toString('base64'), '//4AAQ==');
    strictEqual(bytes.subarray(1).toString('base64url'), '__4AAQ');
    strictEqual(bytes.subarray(1).toString('hex'), 'fffe0001');
    strictEqual(bytes.subarray(1, 4).toString('ucs2'), '\ufeff');
    strictEqual(bytes.toString('ascii'), '{\u007f~\u0000\u0001');
  },
};

export const transcodeEdgeCases = {
  test() {
    strictEqual(
      hex(transcode(Buffer.from('a\u20acb'), 'utf8', 'latin1')),
      '618062'
    );
    strictEqual(
      hex(transcode(Buffer.from([0x80]), 'latin1', 'utf8')),
      'e282ac'
    );
    strictEqual(hex(transcode(Buffer.from([0xff]), 'utf8', 'ascii')), '3f');
    strictEqual(
      hex(transcode(Buffer.from([0x00, 0xd8]), 'utf16le', 'latin1')),
      '3f'
    );
    throws(() => transcode(Buffer.from([0xff, 0x61]), 'utf8', 'utf16le'), {
      name: 'Error',
      message: 'Unable to transcode buffer',
    });
    throws(() => transcode(Buffer.from([0x00, 0xd8]), 'utf16le', 'utf8'), {
      name: 'Error',
      message: 'Expected UTF8 length mismatch',
    });
    throws(() => transcode(new Uint16Array([0x61]), 'utf16le', 'utf8'), {
      name: 'TypeError',
      message:
        "Failed to execute 'transcode' on 'BufferUtil': parameter 1 is not of type 'Uint8Array'.",
    });
    throws(() => transcode(Buffer.from('a'), 'utf8', 'hex'), {
      name: 'Error',
      message: 'Unable to transcode buffer due to unsupported encoding',
    });
  },
};

export const validation = {
  test() {
    strictEqual(isUtf8(Buffer.from([0xed, 0xa0, 0x80])), false); // Surrogate.
    strictEqual(isUtf8(Buffer.from([0xc0, 0x80])), false); // Overlong.
    strictEqual(isUtf8(Buffer.from([0xf4, 0x90, 0x80, 0x80])), false); // > U+10FFFF.
    strictEqual(isAscii(Buffer.from([0x7f])), true);
    strictEqual(isAscii(Buffer.from([0x80])), false);
    throws(() => isAscii(new Uint16Array(1)), {
      name: 'TypeError',
      message:
        "Failed to execute 'isAscii' on 'BufferUtil': parameter 1 is not of type 'Uint8Array'.",
    });
    throws(() => isUtf8(new DataView(new ArrayBuffer(1))), {
      name: 'TypeError',
      message:
        "Failed to execute 'isUtf8' on 'BufferUtil': parameter 1 is not of type 'Uint8Array'.",
    });
  },
};

export const stringDecoderEdgeCases = {
  test() {
    const utf8 = new StringDecoder('utf8');
    strictEqual(utf8.write(Buffer.from([0xe2, 0x82])), '');
    strictEqual(utf8.write(Buffer.from([0x41])), '\ufffdA');
    strictEqual(utf8.end(Buffer.from([0xf0, 0x9f])), '\ufffd');

    const base64 = new StringDecoder('base64');
    strictEqual(base64.write(Buffer.from('ab')), '');
    strictEqual(base64.write(Buffer.from('cde')), 'YWJj');
    strictEqual(base64.end(), 'ZGU=');

    const utf16 = new StringDecoder('utf16le');
    strictEqual(utf16.write(Buffer.from([0x3d])), '');
    strictEqual(utf16.write(Buffer.from([0xd8, 0x00])), '\ud83d');
    strictEqual(utf16.write(Buffer.from([0xde, 0x41])), '\ude00');
    strictEqual(utf16.end(), '');
  },
};
