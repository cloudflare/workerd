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
  deepStrictEqual,
  notDeepStrictEqual,
  notStrictEqual,
  ok,
  strictEqual,
  throws,
} from 'node:assert';
import util from 'node:util';

import {
  Buffer,
  File,
  SlowBuffer,
  constants,
  isAscii,
  isUtf8,
  kMaxLength,
  kStringMaxLength,
  transcode,
} from 'node:buffer';

import * as buffer from 'node:buffer';
if (
  buffer.Buffer !== Buffer ||
  buffer.SlowBuffer !== SlowBuffer ||
  buffer.kMaxLength !== kMaxLength ||
  buffer.kStringMaxLength !== kStringMaxLength ||
  buffer.constants !== constants
) {
  throw new Error('Incorrect default exports');
}

const { MAX_LENGTH, MAX_STRING_LENGTH } = constants;

export const simpleAlloc = {
  // test-buffer-alloc.js
  test(ctrl, env, ctx) {
    const b = Buffer.allocUnsafe(1024);
    strictEqual(b.length, 1024);

    b[0] = -1;
    strictEqual(b[0], 255);

    for (let i = 0; i < 1024; i++) {
      b[i] = i % 256;
    }

    for (let i = 0; i < 1024; i++) {
      strictEqual(i % 256, b[i]);
    }

    const c = Buffer.allocUnsafe(512);
    strictEqual(c.length, 512);

    const d = Buffer.from([]);
    strictEqual(d.length, 0);
  },
};

export const offsetProperties = {
  test(ctrl, env, ctx) {
    const b = Buffer.alloc(128);
    strictEqual(b.length, 128);
    strictEqual(b.byteOffset, 0);
    strictEqual(b.offset, 0);
  },
};

export const bufferFromUint8Array = {
  test(ctrl, env, ctx) {
    {
      const ui8 = new Uint8Array(4).fill(42);
      const e = Buffer.from(ui8);
      for (const [index, value] of e.entries()) {
        strictEqual(value, ui8[index]);
      }
    }

    {
      const ui8 = new Uint8Array(4).fill(42);
      const e = Buffer(ui8);
      for (const [key, value] of e.entries()) {
        strictEqual(value, ui8[key]);
      }
    }
  },
};

export const bufferFromUint32Array = {
  test(ctrl, env, ctx) {
    {
      const ui32 = new Uint32Array(4).fill(42);
      const e = Buffer.from(ui32);
      for (const [index, value] of e.entries()) {
        strictEqual(value, ui32[index]);
      }
    }
    {
      const ui32 = new Uint32Array(4).fill(42);
      const e = Buffer(ui32);
      for (const [key, value] of e.entries()) {
        strictEqual(value, ui32[key]);
      }
    }
  },
};

export const invalidEncodingForToString = {
  test(ctrl, env, ctx) {
    const b = Buffer.allocUnsafe(10);
    // Test invalid encoding for Buffer.toString
    throws(() => b.toString('invalid'), /Unknown encoding: invalid/);
    // // Invalid encoding for Buffer.write
    throws(
      () => b.write('test string', 0, 5, 'invalid'),
      /Unknown encoding: invalid/
    );
    // Unsupported arguments for Buffer.write

    throws(() => b.write('test', 'utf8', 0), { code: 'ERR_INVALID_ARG_TYPE' });
  },
};

export const toStringWithUndefinedEncoding = {
  test(ctrl, env, ctx) {
    // Test that Buffer.toString with undefined encoding defaults to utf8
    const buf = Buffer.from('hello world');
    strictEqual(buf.toString(undefined), 'hello world');
    strictEqual(buf.toString(undefined), buf.toString('utf8'));

    // Test with UTF-8 characters
    const utf8Buf = Buffer.from('¡hέlló wôrld!');
    strictEqual(utf8Buf.toString(undefined), '¡hέlló wôrld!');
    strictEqual(utf8Buf.toString(undefined), utf8Buf.toString('utf8'));

    // Test with start and end parameters
    const sliceBuf = Buffer.from('hello world');
    strictEqual(sliceBuf.toString(undefined, 0, 5), 'hello');
    strictEqual(sliceBuf.toString(undefined, 6), 'world');
    strictEqual(
      sliceBuf.toString(undefined, 0, 5),
      sliceBuf.toString('utf8', 0, 5)
    );
  },
};

export const zeroLengthBuffers = {
  test(ctrl, env, ctx) {
    Buffer.from('');
    Buffer.from('', 'ascii');
    Buffer.from('', 'latin1');
    Buffer.alloc(0);
    Buffer.allocUnsafe(0);
    new Buffer('');
    new Buffer('', 'ascii');
    new Buffer('', 'latin1');
    new Buffer('', 'binary');
    Buffer(0);
  },
};

export const outOfBoundsWrites = {
  test(ctrl, env, ctx) {
    const outOfRangeError = {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    };

    const b = Buffer.alloc(1024);

    // Try to write a 0-length string beyond the end of b
    throws(() => b.write('', 2048), outOfRangeError);

    // Throw when writing to negative offset
    throws(() => b.write('a', -1), outOfRangeError);

    // Throw when writing past bounds from the pool
    throws(() => b.write('a', 2048), outOfRangeError);

    // Throw when writing to negative offset
    throws(() => b.write('a', -1), outOfRangeError);

    // Try to copy 0 bytes worth of data into an empty buffer
    b.copy(Buffer.alloc(0), 0, 0, 0);

    // Try to copy 0 bytes past the end of the target buffer
    b.copy(Buffer.alloc(0), 1, 1, 1);
    b.copy(Buffer.alloc(1), 1, 1, 1);

    // Try to copy 0 bytes from past the end of the source buffer
    b.copy(Buffer.alloc(1), 0, 2048, 2048);

    Buffer.alloc(1).write('', 1, 0);
  },
};

export const smartDefaults = {
  test(ctrl, env, ctx) {
    const writeTest = Buffer.from('abcdes');
    writeTest.write('n', 'ascii');
    throws(() => writeTest.write('o', '1', 'ascii'), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
    writeTest.write('o', 1, 'ascii');
    writeTest.write('d', 2, 'ascii');
    writeTest.write('e', 3, 'ascii');
    writeTest.write('j', 4, 'ascii');
    strictEqual(writeTest.toString(), 'nodejs');
  },
};

export const asciiSlice = {
  test(ctrl, env, ctx) {
    const b = Buffer.alloc(1024);

    {
      const asciiString = 'hello world';

      for (let i = 0; i < asciiString.length; i++) {
        b[i] = asciiString.charCodeAt(i);
      }
      const asciiSlice = b.toString('ascii', 0, asciiString.length);
      strictEqual(asciiString, asciiSlice);
    }

    {
      const asciiString = 'hello world';
      const offset = 100;

      strictEqual(asciiString.length, b.write(asciiString, offset, 'ascii'));
      const asciiSlice = b.toString(
        'ascii',
        offset,
        offset + asciiString.length
      );
      strictEqual(asciiString, asciiSlice);
    }

    {
      const asciiString = 'hello world';
      const offset = 100;

      const sliceA = b.slice(offset, offset + asciiString.length);
      const sliceB = b.slice(offset, offset + asciiString.length);
      for (let i = 0; i < asciiString.length; i++) {
        strictEqual(sliceA[i], sliceB[i]);
      }
    }
  },
};

export const utf8Slice = {
  test(ctrl, env, ctx) {
    const b = Buffer.alloc(1024);
    {
      const utf8String = '¡hέlló wôrld!';
      const offset = 100;

      b.write(utf8String, 0, Buffer.byteLength(utf8String), 'utf8');
      let utf8Slice = b.toString('utf8', 0, Buffer.byteLength(utf8String));
      strictEqual(utf8String, utf8Slice);

      strictEqual(
        Buffer.byteLength(utf8String),
        b.write(utf8String, offset, 'utf8')
      );
      utf8Slice = b.toString(
        'utf8',
        offset,
        offset + Buffer.byteLength(utf8String)
      );
      strictEqual(utf8String, utf8Slice);

      const sliceA = b.slice(offset, offset + Buffer.byteLength(utf8String));
      const sliceB = b.slice(offset, offset + Buffer.byteLength(utf8String));
      for (let i = 0; i < Buffer.byteLength(utf8String); i++) {
        strictEqual(sliceA[i], sliceB[i]);
      }
    }
    {
      const slice = b.slice(100, 150);
      strictEqual(slice.length, 50);
      for (let i = 0; i < 50; i++) {
        strictEqual(b[100 + i], slice[i]);
      }
    }

    {
      // Make sure only top level parent propagates from allocPool
      // (We don't actually implement pooling, this is just carried over from Node.js)
      const b = Buffer.allocUnsafe(5);
      const c = b.slice(0, 4);
      const d = c.slice(0, 2);
      strictEqual(b.parent, c.parent);
      strictEqual(b.parent, d.parent);
    }

    {
      // Also from a non-pooled instance
      const b = Buffer.allocUnsafeSlow(5);
      const c = b.slice(0, 4);
      const d = c.slice(0, 2);
      strictEqual(c.parent, d.parent);
    }

    {
      // Bug regression test
      const testValue = '\u00F6\u65E5\u672C\u8A9E'; // ö日本語
      const buffer = Buffer.allocUnsafe(32);
      const size = buffer.write(testValue, 0, 'utf8');
      const slice = buffer.toString('utf8', 0, size);
      strictEqual(slice, testValue);
    }

    {
      // Test triple  slice
      const a = Buffer.allocUnsafe(8);
      for (let i = 0; i < 8; i++) a[i] = i;
      const b = a.slice(4, 8);
      strictEqual(b[0], 4);
      strictEqual(b[1], 5);
      strictEqual(b[2], 6);
      strictEqual(b[3], 7);
      const c = b.slice(2, 4);
      strictEqual(c[0], 6);
      strictEqual(c[1], 7);
    }
  },
};

export const bufferFrom = {
  test(ctrl, env, ctx) {
    {
      const d = Buffer.from([23, 42, 255]);
      strictEqual(d.length, 3);
      strictEqual(d[0], 23);
      strictEqual(d[1], 42);
      strictEqual(d[2], 255);
      deepStrictEqual(d, Buffer.from(d));
    }
    {
      // Test for proper UTF-8 Encoding
      const e = Buffer.from('über');
      deepStrictEqual(e, Buffer.from([195, 188, 98, 101, 114]));
    }
    {
      // Test for proper ascii Encoding, length should be 4
      const f = Buffer.from('über', 'ascii');
      deepStrictEqual(f, Buffer.from([252, 98, 101, 114]));
    }
    ['ucs2', 'ucs-2', 'utf16le', 'utf-16le'].forEach((encoding) => {
      {
        // Test for proper UTF16LE encoding, length should be 8
        const f = Buffer.from('über', encoding);
        deepStrictEqual(f, Buffer.from([252, 0, 98, 0, 101, 0, 114, 0]));
      }

      {
        // Length should be 12
        const f = Buffer.from('привет', encoding);
        deepStrictEqual(
          f,
          Buffer.from([63, 4, 64, 4, 56, 4, 50, 4, 53, 4, 66, 4])
        );
        strictEqual(f.toString(encoding), 'привет');
      }

      {
        const f = Buffer.from([0, 0, 0, 0, 0]);
        strictEqual(f.length, 5);
        const size = f.write('あいうえお', encoding);
        strictEqual(size, 4);
        deepStrictEqual(f, Buffer.from([0x42, 0x30, 0x44, 0x30, 0x00]));
      }

      {
        const f = Buffer.from('\uD83D\uDC4D', 'utf-16le'); // THUMBS UP SIGN (U+1F44D)
        strictEqual(f.length, 4);
        deepStrictEqual(f, Buffer.from('3DD84DDC', 'hex'));
      }

      {
        const arrayIsh = { 0: 0, 1: 1, 2: 2, 3: 3, length: 4 };
        let g = Buffer.from(arrayIsh);
        deepStrictEqual(g, Buffer.from([0, 1, 2, 3]));
        const strArrayIsh = { 0: '0', 1: '1', 2: '2', 3: '3', length: 4 };
        g = Buffer.from(strArrayIsh);
        deepStrictEqual(g, Buffer.from([0, 1, 2, 3]));
      }
    });

    {
      const checkString = 'test';

      const check = Buffer.from(checkString);

      class MyString extends String {
        constructor() {
          super(checkString);
        }
      }

      class MyPrimitive {
        [Symbol.toPrimitive]() {
          return checkString;
        }
      }

      class MyBadPrimitive {
        [Symbol.toPrimitive]() {
          return 1;
        }
      }

      deepStrictEqual(Buffer.from(new String(checkString)), check);
      deepStrictEqual(Buffer.from(new MyString()), check);
      deepStrictEqual(Buffer.from(new MyPrimitive()), check);

      [
        {},
        new Boolean(true),
        {
          valueOf() {
            return null;
          },
        },
        {
          valueOf() {
            return undefined;
          },
        },
        { valueOf: null },
        { __proto__: null },
        new Number(true),
        new MyBadPrimitive(),
        Symbol(),
        5n,
        (one, two, three) => {},
        undefined,
        null,
      ].forEach((input) => {
        const errObj = {
          name: 'TypeError',
        };
        throws(() => Buffer.from(input), errObj);
        throws(() => Buffer.from(input, 'hex'), errObj);
      });

      Buffer.allocUnsafe(10); // Should not throw.
      Buffer.from('deadbeaf', 'hex'); // Should not throw.
    }
  },
};

export const base64 = {
  test(ctrl, env, ctx) {
    const base64flavors = ['base64', 'base64url'];
    {
      strictEqual(Buffer.from('Man').toString('base64'), 'TWFu');
      strictEqual(Buffer.from('Woman').toString('base64'), 'V29tYW4=');
      strictEqual(Buffer.from('Man').toString('base64url'), 'TWFu');
      strictEqual(Buffer.from('Woman').toString('base64url'), 'V29tYW4');
    }

    {
      const expected = [0xff, 0xff, 0xbe, 0xff, 0xef, 0xbf, 0xfb, 0xef, 0xff];
      deepStrictEqual(
        Buffer.from('//++/++/++//', 'base64'),
        Buffer.from(expected)
      );
      deepStrictEqual(
        Buffer.from('__--_--_--__', 'base64'),
        Buffer.from(expected)
      );
      deepStrictEqual(
        Buffer.from('//++/++/++//', 'base64url'),
        Buffer.from(expected)
      );
      deepStrictEqual(
        Buffer.from('__--_--_--__', 'base64url'),
        Buffer.from(expected)
      );
    }

    {
      // Test that regular and URL-safe base64 both work both ways with padding
      const expected = [
        0xff, 0xff, 0xbe, 0xff, 0xef, 0xbf, 0xfb, 0xef, 0xff, 0xfb,
      ];
      deepStrictEqual(
        Buffer.from('//++/++/++//+w==', 'base64'),
        Buffer.from(expected)
      );
      deepStrictEqual(
        Buffer.from('//++/++/++//+w==', 'base64'),
        Buffer.from(expected)
      );
      deepStrictEqual(
        Buffer.from('//++/++/++//+w==', 'base64url'),
        Buffer.from(expected)
      );
      deepStrictEqual(
        Buffer.from('//++/++/++//+w==', 'base64url'),
        Buffer.from(expected)
      );
    }

    {
      // big example
      const quote =
        'Man is distinguished, not only by his reason, but by this ' +
        'singular passion from other animals, which is a lust ' +
        'of the mind, that by a perseverance of delight in the ' +
        'continued and indefatigable generation of knowledge, ' +
        'exceeds the short vehemence of any carnal pleasure.';
      const expected =
        'TWFuIGlzIGRpc3Rpbmd1aXNoZWQsIG5vdCBvbmx5IGJ5IGhpcyByZWFzb' +
        '24sIGJ1dCBieSB0aGlzIHNpbmd1bGFyIHBhc3Npb24gZnJvbSBvdGhlci' +
        'BhbmltYWxzLCB3aGljaCBpcyBhIGx1c3Qgb2YgdGhlIG1pbmQsIHRoYXQ' +
        'gYnkgYSBwZXJzZXZlcmFuY2Ugb2YgZGVsaWdodCBpbiB0aGUgY29udGlu' +
        'dWVkIGFuZCBpbmRlZmF0aWdhYmxlIGdlbmVyYXRpb24gb2Yga25vd2xlZ' +
        'GdlLCBleGNlZWRzIHRoZSBzaG9ydCB2ZWhlbWVuY2Ugb2YgYW55IGNhcm' +
        '5hbCBwbGVhc3VyZS4=';
      strictEqual(Buffer.from(quote).toString('base64'), expected);
      strictEqual(
        Buffer.from(quote).toString('base64url'),
        expected.replaceAll('+', '-').replaceAll('/', '_').replaceAll('=', '')
      );

      base64flavors.forEach((encoding) => {
        let b = Buffer.allocUnsafe(1024);
        let bytesWritten = b.write(expected, 0, encoding);
        strictEqual(quote.length, bytesWritten);
        strictEqual(quote, b.toString('ascii', 0, quote.length));

        // Check that the base64 decoder ignores whitespace
        const expectedWhite =
          `${expected.slice(0, 60)} \n` +
          `${expected.slice(60, 120)} \n` +
          `${expected.slice(120, 180)} \n` +
          `${expected.slice(180, 240)} \n` +
          `${expected.slice(240, 300)}\n` +
          `${expected.slice(300, 360)}\n`;
        b = Buffer.allocUnsafe(1024);
        bytesWritten = b.write(expectedWhite, 0, encoding);
        strictEqual(quote.length, bytesWritten);
        strictEqual(quote, b.toString('ascii', 0, quote.length));

        // Check that the base64 decoder on the constructor works
        // even in the presence of whitespace.
        b = Buffer.from(expectedWhite, encoding);
        strictEqual(quote.length, b.length);
        strictEqual(quote, b.toString('ascii', 0, quote.length));

        // Check that the base64 decoder ignores illegal chars
        const expectedIllegal =
          expected.slice(0, 60) +
          ' \x80' +
          expected.slice(60, 120) +
          ' \xff' +
          expected.slice(120, 180) +
          ' \x00' +
          expected.slice(180, 240) +
          ' \x98' +
          expected.slice(240, 300) +
          '\x03' +
          expected.slice(300, 360);
        b = Buffer.from(expectedIllegal, encoding);
        strictEqual(quote.length, b.length);
        strictEqual(quote, b.toString('ascii', 0, quote.length));
      });
    }

    base64flavors.forEach((encoding) => {
      strictEqual(Buffer.from('', encoding).toString(), '');
      strictEqual(Buffer.from('K', encoding).toString(), '');

      // multiple-of-4 with padding
      strictEqual(Buffer.from('Kg==', encoding).toString(), '*');
      strictEqual(Buffer.from('Kio=', encoding).toString(), '*'.repeat(2));
      strictEqual(Buffer.from('Kioq', encoding).toString(), '*'.repeat(3));
      strictEqual(Buffer.from('KioqKg==', encoding).toString(), '*'.repeat(4));
      strictEqual(Buffer.from('KioqKio=', encoding).toString(), '*'.repeat(5));
      strictEqual(Buffer.from('KioqKioq', encoding).toString(), '*'.repeat(6));
      strictEqual(
        Buffer.from('KioqKioqKg==', encoding).toString(),
        '*'.repeat(7)
      );
      strictEqual(
        Buffer.from('KioqKioqKio=', encoding).toString(),
        '*'.repeat(8)
      );
      strictEqual(
        Buffer.from('KioqKioqKioq', encoding).toString(),
        '*'.repeat(9)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKg==', encoding).toString(),
        '*'.repeat(10)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKio=', encoding).toString(),
        '*'.repeat(11)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioq', encoding).toString(),
        '*'.repeat(12)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKg==', encoding).toString(),
        '*'.repeat(13)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKio=', encoding).toString(),
        '*'.repeat(14)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKioq', encoding).toString(),
        '*'.repeat(15)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKioqKg==', encoding).toString(),
        '*'.repeat(16)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKioqKio=', encoding).toString(),
        '*'.repeat(17)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKioqKioq', encoding).toString(),
        '*'.repeat(18)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKioqKioqKg==', encoding).toString(),
        '*'.repeat(19)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKioqKioqKio=', encoding).toString(),
        '*'.repeat(20)
      );

      // No padding, not a multiple of 4
      strictEqual(Buffer.from('Kg', encoding).toString(), '*');
      strictEqual(Buffer.from('Kio', encoding).toString(), '*'.repeat(2));
      strictEqual(Buffer.from('KioqKg', encoding).toString(), '*'.repeat(4));
      strictEqual(Buffer.from('KioqKio', encoding).toString(), '*'.repeat(5));
      strictEqual(
        Buffer.from('KioqKioqKg', encoding).toString(),
        '*'.repeat(7)
      );
      strictEqual(
        Buffer.from('KioqKioqKio', encoding).toString(),
        '*'.repeat(8)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKg', encoding).toString(),
        '*'.repeat(10)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKio', encoding).toString(),
        '*'.repeat(11)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKg', encoding).toString(),
        '*'.repeat(13)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKio', encoding).toString(),
        '*'.repeat(14)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKioqKg', encoding).toString(),
        '*'.repeat(16)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKioqKio', encoding).toString(),
        '*'.repeat(17)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKioqKioqKg', encoding).toString(),
        '*'.repeat(19)
      );
      strictEqual(
        Buffer.from('KioqKioqKioqKioqKioqKioqKio', encoding).toString(),
        '*'.repeat(20)
      );
    });

    // Handle padding graciously, multiple-of-4 or not
    strictEqual(
      Buffer.from('72INjkR5fchcxk9+VgdGPFJDxUBFR5/rMFsghgxADiw==', 'base64')
        .length,
      32
    );
    strictEqual(
      Buffer.from('72INjkR5fchcxk9-VgdGPFJDxUBFR5_rMFsghgxADiw==', 'base64url')
        .length,
      32
    );
    strictEqual(
      Buffer.from('72INjkR5fchcxk9+VgdGPFJDxUBFR5/rMFsghgxADiw=', 'base64')
        .length,
      32
    );
    strictEqual(
      Buffer.from('72INjkR5fchcxk9-VgdGPFJDxUBFR5_rMFsghgxADiw=', 'base64url')
        .length,
      32
    );
    strictEqual(
      Buffer.from('72INjkR5fchcxk9+VgdGPFJDxUBFR5/rMFsghgxADiw', 'base64')
        .length,
      32
    );
    strictEqual(
      Buffer.from('72INjkR5fchcxk9-VgdGPFJDxUBFR5_rMFsghgxADiw', 'base64url')
        .length,
      32
    );
    strictEqual(
      Buffer.from('w69jACy6BgZmaFvv96HG6MYksWytuZu3T1FvGnulPg==', 'base64')
        .length,
      31
    );
    strictEqual(
      Buffer.from('w69jACy6BgZmaFvv96HG6MYksWytuZu3T1FvGnulPg==', 'base64url')
        .length,
      31
    );
    strictEqual(
      Buffer.from('w69jACy6BgZmaFvv96HG6MYksWytuZu3T1FvGnulPg=', 'base64')
        .length,
      31
    );
    strictEqual(
      Buffer.from('w69jACy6BgZmaFvv96HG6MYksWytuZu3T1FvGnulPg=', 'base64url')
        .length,
      31
    );
    strictEqual(
      Buffer.from('w69jACy6BgZmaFvv96HG6MYksWytuZu3T1FvGnulPg', 'base64')
        .length,
      31
    );
    strictEqual(
      Buffer.from('w69jACy6BgZmaFvv96HG6MYksWytuZu3T1FvGnulPg', 'base64url')
        .length,
      31
    );

    {
      // This string encodes single '.' character in UTF-16
      const dot = Buffer.from('//4uAA==', 'base64');
      strictEqual(dot[0], 0xff);
      strictEqual(dot[1], 0xfe);
      strictEqual(dot[2], 0x2e);
      strictEqual(dot[3], 0x00);
      strictEqual(dot.toString('base64'), '//4uAA==');
    }

    {
      // This string encodes single '.' character in UTF-16
      const dot = Buffer.from('//4uAA', 'base64url');
      strictEqual(dot[0], 0xff);
      strictEqual(dot[1], 0xfe);
      strictEqual(dot[2], 0x2e);
      strictEqual(dot[3], 0x00);
      strictEqual(dot.toString('base64url'), '__4uAA');
    }

    {
      // Writing base64 at a position > 0 should not mangle the result.
      //
      // https://github.com/joyent/node/issues/402
      const segments = ['TWFkbmVzcz8h', 'IFRoaXM=', 'IGlz', 'IG5vZGUuanMh'];
      const b = Buffer.allocUnsafe(64);
      let pos = 0;

      for (let i = 0; i < segments.length; ++i) {
        pos += b.write(segments[i], pos, 'base64');
      }
      strictEqual(b.toString('latin1', 0, pos), 'Madness?! This is node.js!');
    }

    {
      // Writing base64url at a position > 0 should not mangle the result.
      //
      // https://github.com/joyent/node/issues/402
      const segments = ['TWFkbmVzcz8h', 'IFRoaXM', 'IGlz', 'IG5vZGUuanMh'];
      const b = Buffer.allocUnsafe(64);
      let pos = 0;

      for (let i = 0; i < segments.length; ++i) {
        pos += b.write(segments[i], pos, 'base64url');
      }
      strictEqual(b.toString('latin1', 0, pos), 'Madness?! This is node.js!');
    }

    // Regression test for https://github.com/nodejs/node/issues/3496.
    strictEqual(Buffer.from('=bad'.repeat(1e4), 'base64').length, 0);

    // // Regression test for https://github.com/nodejs/node/issues/11987.
    deepStrictEqual(Buffer.from('w0  ', 'base64'), Buffer.from('w0', 'base64'));

    // // Regression test for https://github.com/nodejs/node/issues/13657.
    deepStrictEqual(
      Buffer.from(' YWJvcnVtLg', 'base64'),
      Buffer.from('YWJvcnVtLg', 'base64')
    );
  },
};

export const hex = {
  test(ctrl, env, ctx) {
    {
      // test hex toString
      const hexb = Buffer.allocUnsafe(256);
      for (let i = 0; i < 256; i++) {
        hexb[i] = i;
      }
      const hexStr = hexb.toString('hex');
      strictEqual(
        hexStr,
        '000102030405060708090a0b0c0d0e0f' +
          '101112131415161718191a1b1c1d1e1f' +
          '202122232425262728292a2b2c2d2e2f' +
          '303132333435363738393a3b3c3d3e3f' +
          '404142434445464748494a4b4c4d4e4f' +
          '505152535455565758595a5b5c5d5e5f' +
          '606162636465666768696a6b6c6d6e6f' +
          '707172737475767778797a7b7c7d7e7f' +
          '808182838485868788898a8b8c8d8e8f' +
          '909192939495969798999a9b9c9d9e9f' +
          'a0a1a2a3a4a5a6a7a8a9aaabacadaeaf' +
          'b0b1b2b3b4b5b6b7b8b9babbbcbdbebf' +
          'c0c1c2c3c4c5c6c7c8c9cacbcccdcecf' +
          'd0d1d2d3d4d5d6d7d8d9dadbdcdddedf' +
          'e0e1e2e3e4e5e6e7e8e9eaebecedeeef' +
          'f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff'
      );

      const hexb2 = Buffer.from(hexStr, 'hex');
      for (let i = 0; i < 256; i++) {
        strictEqual(hexb2[i], hexb[i]);
      }
    }

    // Test single hex character is discarded.
    strictEqual(Buffer.from('A', 'hex').length, 0);

    // Test that if a trailing character is discarded, rest of string is processed.
    deepStrictEqual(Buffer.from('Abx', 'hex'), Buffer.from('Ab', 'hex'));

    // Test single base64 char encodes as 0.
    strictEqual(Buffer.from('A', 'base64').length, 0);

    {
      // Test an invalid slice end.
      const b = Buffer.from([1, 2, 3, 4, 5]);
      const b2 = b.toString('hex', 1, 10000);
      const b3 = b.toString('hex', 1, 5);
      const b4 = b.toString('hex', 1);
      strictEqual(b2, b3);
      strictEqual(b2, b4);
    }
  },
};

export const slicing = {
  test(ctrl, env, ctx) {
    function buildBuffer(data) {
      if (Array.isArray(data)) {
        const buffer = Buffer.allocUnsafe(data.length);
        data.forEach((v, k) => (buffer[k] = v));
        return buffer;
      }
      return null;
    }

    const x = buildBuffer([
      0x81, 0xa3, 0x66, 0x6f, 0x6f, 0xa3, 0x62, 0x61, 0x72,
    ]);

    {
      const z = x.slice(4);
      strictEqual(z.length, 5);
      strictEqual(z[0], 0x6f);
      strictEqual(z[1], 0xa3);
      strictEqual(z[2], 0x62);
      strictEqual(z[3], 0x61);
      strictEqual(z[4], 0x72);
    }

    {
      const z = x.slice(0);
      strictEqual(z.length, x.length);
    }

    {
      const z = x.slice(0, 4);
      strictEqual(z.length, 4);
      strictEqual(z[0], 0x81);
      strictEqual(z[1], 0xa3);
    }

    {
      const z = x.slice(0, 9);
      strictEqual(z.length, 9);
    }

    {
      const z = x.slice(1, 4);
      strictEqual(z.length, 3);
      strictEqual(z[0], 0xa3);
    }

    {
      const z = x.slice(2, 4);
      strictEqual(z.length, 2);
      strictEqual(z[0], 0x66);
      strictEqual(z[1], 0x6f);
    }
  },
};

export const writing = {
  test(ctrl, env, ctx) {
    ['ucs2', 'ucs-2', 'utf16le', 'utf-16le'].forEach((encoding) => {
      const b = Buffer.allocUnsafe(10);
      b.write('あいうえお', encoding);
      strictEqual(b.toString(encoding), 'あいうえお');
    });

    ['ucs2', 'ucs-2', 'utf16le', 'utf-16le'].forEach((encoding) => {
      const b = Buffer.allocUnsafe(11);
      b.write('あいうえお', 1, encoding);
      strictEqual(b.toString(encoding, 1), 'あいうえお');
    });

    {
      // latin1 encoding should write only one byte per character.
      const b = Buffer.from([0xde, 0xad, 0xbe, 0xef]);
      let s = String.fromCharCode(0xffff);
      b.write(s, 0, 'latin1');
      strictEqual(b[0], 0xff);
      strictEqual(b[1], 0xad);
      strictEqual(b[2], 0xbe);
      strictEqual(b[3], 0xef);
      s = String.fromCharCode(0xaaee);
      b.write(s, 0, 'latin1');
      strictEqual(b[0], 0xee);
      strictEqual(b[1], 0xad);
      strictEqual(b[2], 0xbe);
      strictEqual(b[3], 0xef);
    }

    {
      // Binary encoding should write only one byte per character.
      const b = Buffer.from([0xde, 0xad, 0xbe, 0xef]);
      let s = String.fromCharCode(0xffff);
      b.write(s, 0, 'latin1');
      strictEqual(b[0], 0xff);
      strictEqual(b[1], 0xad);
      strictEqual(b[2], 0xbe);
      strictEqual(b[3], 0xef);
      s = String.fromCharCode(0xaaee);
      b.write(s, 0, 'latin1');
      strictEqual(b[0], 0xee);
      strictEqual(b[1], 0xad);
      strictEqual(b[2], 0xbe);
      strictEqual(b[3], 0xef);
    }

    {
      const buf = Buffer.allocUnsafe(2);
      strictEqual(buf.write(''), 0); // 0bytes
      strictEqual(buf.write('\0'), 1); // 1byte (v8 adds null terminator)
      strictEqual(buf.write('a\0'), 2); // 1byte * 2
      strictEqual(buf.write('あ'), 0); // 3bytes
      strictEqual(buf.write('\0あ'), 1); // 1byte + 3bytes
      strictEqual(buf.write('\0\0あ'), 2); // 1byte * 2 + 3bytes
    }

    {
      const buf = Buffer.allocUnsafe(10);
      strictEqual(buf.write('あいう'), 9); // 3bytes * 3 (v8 adds null term.)
      strictEqual(buf.write('あいう\0'), 10); // 3bytes * 3 + 1byte
    }

    {
      // https://github.com/nodejs/node-v0.x-archive/issues/243
      // Test write() with maxLength
      const buf = Buffer.allocUnsafe(4);
      buf.fill(0xff);
      strictEqual(buf.write('abcd', 1, 2, 'utf8'), 2);
      strictEqual(buf[0], 0xff);
      strictEqual(buf[1], 0x61);
      strictEqual(buf[2], 0x62);
      strictEqual(buf[3], 0xff);

      buf.fill(0xff);
      strictEqual(buf.write('abcd', 1, 4), 3);
      strictEqual(buf[0], 0xff);
      strictEqual(buf[1], 0x61);
      strictEqual(buf[2], 0x62);
      strictEqual(buf[3], 0x63);

      buf.fill(0xff);
      strictEqual(buf.write('abcd', 1, 2, 'utf8'), 2);
      strictEqual(buf[0], 0xff);
      strictEqual(buf[1], 0x61);
      strictEqual(buf[2], 0x62);
      strictEqual(buf[3], 0xff);

      buf.fill(0xff);
      strictEqual(buf.write('abcdef', 1, 2, 'hex'), 2);
      strictEqual(buf[0], 0xff);
      strictEqual(buf[1], 0xab);
      strictEqual(buf[2], 0xcd);
      strictEqual(buf[3], 0xff);

      ['ucs2', 'ucs-2', 'utf16le', 'utf-16le'].forEach((encoding) => {
        buf.fill(0xff);
        strictEqual(buf.write('abcd', 0, 2, encoding), 2);
        strictEqual(buf[0], 0x61);
        strictEqual(buf[1], 0x00);
        strictEqual(buf[2], 0xff);
        strictEqual(buf[3], 0xff);
      });
    }

    {
      // Test offset returns are correct
      const b = Buffer.allocUnsafe(16);
      strictEqual(b.writeUInt32LE(0, 0), 4);
      strictEqual(b.writeUInt16LE(0, 4), 6);
      strictEqual(b.writeUInt8(0, 6), 7);
      strictEqual(b.writeInt8(0, 7), 8);
      strictEqual(b.writeDoubleLE(0, 8), 16);
    }

    {
      // Test for buffer overrun
      const buf = Buffer.from([0, 0, 0, 0, 0]); // length: 5
      const sub = buf.slice(0, 4); // length: 4
      strictEqual(sub.write('12345', 'latin1'), 4);
      strictEqual(buf[4], 0);
      strictEqual(sub.write('12345', 'binary'), 4);
      strictEqual(buf[4], 0);
    }

    // Test for common write(U)IntLE/BE
    {
      let buf = Buffer.allocUnsafe(3);
      buf.writeUIntLE(0x123456, 0, 3);
      deepStrictEqual(buf.toJSON().data, [0x56, 0x34, 0x12]);
      strictEqual(buf.readUIntLE(0, 3), 0x123456);

      buf.fill(0xff);
      buf.writeUIntBE(0x123456, 0, 3);
      deepStrictEqual(buf.toJSON().data, [0x12, 0x34, 0x56]);
      strictEqual(buf.readUIntBE(0, 3), 0x123456);

      buf.fill(0xff);
      buf.writeIntLE(0x123456, 0, 3);
      deepStrictEqual(buf.toJSON().data, [0x56, 0x34, 0x12]);
      strictEqual(buf.readIntLE(0, 3), 0x123456);

      buf.fill(0xff);
      buf.writeIntBE(0x123456, 0, 3);
      deepStrictEqual(buf.toJSON().data, [0x12, 0x34, 0x56]);
      strictEqual(buf.readIntBE(0, 3), 0x123456);

      buf.fill(0xff);
      buf.writeIntLE(-0x123456, 0, 3);
      deepStrictEqual(buf.toJSON().data, [0xaa, 0xcb, 0xed]);
      strictEqual(buf.readIntLE(0, 3), -0x123456);

      buf.fill(0xff);
      buf.writeIntBE(-0x123456, 0, 3);
      deepStrictEqual(buf.toJSON().data, [0xed, 0xcb, 0xaa]);
      strictEqual(buf.readIntBE(0, 3), -0x123456);

      buf.fill(0xff);
      buf.writeIntLE(-0x123400, 0, 3);
      deepStrictEqual(buf.toJSON().data, [0x00, 0xcc, 0xed]);
      strictEqual(buf.readIntLE(0, 3), -0x123400);

      buf.fill(0xff);
      buf.writeIntBE(-0x123400, 0, 3);
      deepStrictEqual(buf.toJSON().data, [0xed, 0xcc, 0x00]);
      strictEqual(buf.readIntBE(0, 3), -0x123400);

      buf.fill(0xff);
      buf.writeIntLE(-0x120000, 0, 3);
      deepStrictEqual(buf.toJSON().data, [0x00, 0x00, 0xee]);
      strictEqual(buf.readIntLE(0, 3), -0x120000);

      buf.fill(0xff);
      buf.writeIntBE(-0x120000, 0, 3);
      deepStrictEqual(buf.toJSON().data, [0xee, 0x00, 0x00]);
      strictEqual(buf.readIntBE(0, 3), -0x120000);

      buf = Buffer.allocUnsafe(5);
      buf.writeUIntLE(0x1234567890, 0, 5);
      deepStrictEqual(buf.toJSON().data, [0x90, 0x78, 0x56, 0x34, 0x12]);
      strictEqual(buf.readUIntLE(0, 5), 0x1234567890);

      buf.fill(0xff);
      buf.writeUIntBE(0x1234567890, 0, 5);
      deepStrictEqual(buf.toJSON().data, [0x12, 0x34, 0x56, 0x78, 0x90]);
      strictEqual(buf.readUIntBE(0, 5), 0x1234567890);

      buf.fill(0xff);
      buf.writeIntLE(0x1234567890, 0, 5);
      deepStrictEqual(buf.toJSON().data, [0x90, 0x78, 0x56, 0x34, 0x12]);
      strictEqual(buf.readIntLE(0, 5), 0x1234567890);

      buf.fill(0xff);
      buf.writeIntBE(0x1234567890, 0, 5);
      deepStrictEqual(buf.toJSON().data, [0x12, 0x34, 0x56, 0x78, 0x90]);
      strictEqual(buf.readIntBE(0, 5), 0x1234567890);

      buf.fill(0xff);
      buf.writeIntLE(-0x1234567890, 0, 5);
      deepStrictEqual(buf.toJSON().data, [0x70, 0x87, 0xa9, 0xcb, 0xed]);
      strictEqual(buf.readIntLE(0, 5), -0x1234567890);

      buf.fill(0xff);
      buf.writeIntBE(-0x1234567890, 0, 5);
      deepStrictEqual(buf.toJSON().data, [0xed, 0xcb, 0xa9, 0x87, 0x70]);
      strictEqual(buf.readIntBE(0, 5), -0x1234567890);

      buf.fill(0xff);
      buf.writeIntLE(-0x0012000000, 0, 5);
      deepStrictEqual(buf.toJSON().data, [0x00, 0x00, 0x00, 0xee, 0xff]);
      strictEqual(buf.readIntLE(0, 5), -0x0012000000);

      buf.fill(0xff);
      buf.writeIntBE(-0x0012000000, 0, 5);
      deepStrictEqual(buf.toJSON().data, [0xff, 0xee, 0x00, 0x00, 0x00]);
      strictEqual(buf.readIntBE(0, 5), -0x0012000000);
    }
  },
};

export const misc = {
  test(ctrl, env, ctx) {
    {
      // https://github.com/nodejs/node-v0.x-archive/pull/1210
      // Test UTF-8 string includes null character
      let buf = Buffer.from('\0');
      strictEqual(buf.length, 1);
      buf = Buffer.from('\0\0');
      strictEqual(buf.length, 2);
    }

    {
      // Test unmatched surrogates not producing invalid utf8 output
      // ef bf bd = utf-8 representation of unicode replacement character
      // see https://codereview.chromium.org/121173009/
      const buf = Buffer.from('ab\ud800cd', 'utf8');
      strictEqual(buf[0], 0x61);
      strictEqual(buf[1], 0x62);
      strictEqual(buf[2], 0xef);
      strictEqual(buf[3], 0xbf);
      strictEqual(buf[4], 0xbd);
      strictEqual(buf[5], 0x63);
      strictEqual(buf[6], 0x64);
    }

    {
      // Test alloc with fill option
      const buf = Buffer.alloc(5, '800A', 'hex');
      strictEqual(buf[0], 128);
      strictEqual(buf[1], 10);
      strictEqual(buf[2], 128);
      strictEqual(buf[3], 10);
      strictEqual(buf[4], 128);
    }

    // Call .fill() first, stops Valgrind warning about uninitialized memory reads.
    Buffer.allocUnsafe(3.3).fill().toString();
    // Throws bad argument error in commit 43cb4ec
    Buffer.alloc(3.3).fill().toString();
    strictEqual(Buffer.allocUnsafe(3.3).length, 3);
    strictEqual(Buffer.from({ length: 3.3 }).length, 3);
    strictEqual(Buffer.from({ length: 'BAM' }).length, 0);

    strictEqual(Buffer.from('99').length, 2);
    strictEqual(Buffer.from('13.37').length, 5);

    // Ensure that the length argument is respected.
    ['ascii', 'utf8', 'hex', 'base64', 'latin1', 'binary'].forEach((enc) => {
      strictEqual(Buffer.allocUnsafe(1).write('aaaaaa', 0, 1, enc), 1);
    });

    {
      // Regression test, guard against buffer overrun in the base64 decoder.
      const a = Buffer.allocUnsafe(3);
      const b = Buffer.from('xxx');
      a.write('aaaaaaaa', 'base64');
      strictEqual(b.toString(), 'xxx');
    }

    // issue GH-3416
    Buffer.from(Buffer.allocUnsafe(0), 0, 0);

    const outOfRangeError = {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    };

    // issue GH-5587
    throws(() => Buffer.alloc(8).writeFloatLE(0, 5), outOfRangeError);
    throws(() => Buffer.alloc(16).writeDoubleLE(0, 9), outOfRangeError);

    // Attempt to overflow buffers, similar to previous bug in array buffers
    throws(
      () => Buffer.allocUnsafe(8).writeFloatLE(0.0, 0xffffffff),
      outOfRangeError
    );
    throws(
      () => Buffer.allocUnsafe(8).writeFloatLE(0.0, 0xffffffff),
      outOfRangeError
    );

    // Ensure negative values can't get past offset
    throws(() => Buffer.allocUnsafe(8).writeFloatLE(0.0, -1), outOfRangeError);
    throws(() => Buffer.allocUnsafe(8).writeFloatLE(0.0, -1), outOfRangeError);

    // Regression test for https://github.com/nodejs/node-v0.x-archive/issues/5482:
    // should throw but not assert in C++ land.
    throws(() => Buffer.from('', 'buffer'), {
      code: 'ERR_UNKNOWN_ENCODING',
      name: 'TypeError',
      message: 'Unknown encoding: buffer',
    });

    // Regression test for https://github.com/nodejs/node-v0.x-archive/issues/6111.
    // Constructing a buffer from another buffer should a) work, and b) not corrupt
    // the source buffer.
    {
      const a = [...Array(128).keys()]; // [0, 1, 2, 3, ... 126, 127]
      const b = Buffer.from(a);
      const c = Buffer.from(b);
      strictEqual(b.length, a.length);
      strictEqual(c.length, a.length);
      for (let i = 0, k = a.length; i < k; ++i) {
        strictEqual(a[i], i);
        strictEqual(b[i], i);
        strictEqual(c[i], i);
      }
    }

    throws(() => Buffer.allocUnsafe(10).copy(), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
      message:
        'The "target" argument must be an instance of Buffer or ' +
        'Uint8Array. Received undefined',
    });

    throws(() => Buffer.from(), {
      name: 'TypeError',
    });
    throws(() => Buffer.from(null), {
      name: 'TypeError',
    });

    // Test prototype getters don't throw
    strictEqual(Buffer.prototype.parent, undefined);
    strictEqual(Buffer.prototype.offset, undefined);
    strictEqual(SlowBuffer.prototype.parent, undefined);
    strictEqual(SlowBuffer.prototype.offset, undefined);

    {
      // Test that large negative Buffer length inputs don't affect the pool offset.
      // Use the fromArrayLike() variant here because it's more lenient
      // about its input and passes the length directly to allocate().
      deepStrictEqual(Buffer.from({ length: -3456 }), Buffer.from(''));
      deepStrictEqual(Buffer.from({ length: -100 }), Buffer.from(''));

      // Check pool offset after that by trying to write string into the pool.
      Buffer.from('abc');
    }

    // Test that ParseArrayIndex handles full uint32
    {
      throws(() => Buffer.from(new ArrayBuffer(0), -1 >>> 0), {
        code: 'ERR_BUFFER_OUT_OF_BOUNDS',
        name: 'RangeError',
        message: '"offset" is outside of buffer bounds',
      });
    }

    // ParseArrayIndex() should reject values that don't fit in a 32 bits size_t.
    throws(() => {
      const a = Buffer.alloc(1);
      const b = Buffer.alloc(1);
      a.copy(b, 0, 0x100000000, 0x100000001);
    }, outOfRangeError);

    // Unpooled buffer (replaces SlowBuffer)
    {
      const ubuf = Buffer.allocUnsafeSlow(10);
      ok(ubuf);
      ok(ubuf.buffer);
      strictEqual(ubuf.buffer.byteLength, 10);
    }

    // Regression test to verify that an empty ArrayBuffer does not throw.
    Buffer.from(new ArrayBuffer());

    throws(
      () => Buffer.alloc({ valueOf: () => 1 }),
      /"size" argument must be of type number/
    );
    throws(
      () => Buffer.alloc({ valueOf: () => -1 }),
      /"size" argument must be of type number/
    );

    strictEqual(Buffer.prototype.toLocaleString, Buffer.prototype.toString);
    {
      const buf = Buffer.from('test');
      strictEqual(buf.toLocaleString(), buf.toString());
    }

    throws(
      () => {
        Buffer.alloc(0x1000, 'This is not correctly encoded', 'hex');
      },
      {
        name: 'TypeError',
      }
    );

    throws(
      () => {
        Buffer.alloc(0x1000, 'c', 'hex');
      },
      {
        name: 'TypeError',
      }
    );

    throws(
      () => {
        Buffer.alloc(1, Buffer.alloc(0));
      },
      {
        code: 'ERR_INVALID_ARG_VALUE',
        name: 'TypeError',
      }
    );

    throws(
      () => {
        Buffer.alloc(40, 'x', 20);
      },
      {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      }
    );
  },
};

export const arrayBuffers = {
  test(ctrl, env, ctx) {
    const LENGTH = 16;

    const ab = new ArrayBuffer(LENGTH);
    const dv = new DataView(ab);
    const ui = new Uint8Array(ab);
    const buf = Buffer.from(ab);

    ok(buf instanceof Buffer);
    strictEqual(buf.parent, buf.buffer);
    strictEqual(buf.buffer, ab);
    strictEqual(buf.length, ab.byteLength);

    buf.fill(0xc);
    for (let i = 0; i < LENGTH; i++) {
      strictEqual(ui[i], 0xc);
      ui[i] = 0xf;
      strictEqual(buf[i], 0xf);
    }

    buf.writeUInt32LE(0xf00, 0);
    buf.writeUInt32BE(0xb47, 4);
    buf.writeDoubleLE(3.1415, 8);

    strictEqual(dv.getUint32(0, true), 0xf00);
    strictEqual(dv.getUint32(4), 0xb47);
    strictEqual(dv.getFloat64(8, true), 3.1415);

    // Now test protecting users from doing stupid things

    throws(
      function () {
        function AB() {}
        Object.setPrototypeOf(AB, ArrayBuffer);
        Object.setPrototypeOf(AB.prototype, ArrayBuffer.prototype);
        Buffer.from(new AB());
      },
      {
        name: 'TypeError',
      }
    );

    // Test the byteOffset and length arguments
    {
      const ab = new Uint8Array(5);
      ab[0] = 1;
      ab[1] = 2;
      ab[2] = 3;
      ab[3] = 4;
      ab[4] = 5;
      const buf = Buffer.from(ab.buffer, 1, 3);
      strictEqual(buf.length, 3);
      strictEqual(buf[0], 2);
      strictEqual(buf[1], 3);
      strictEqual(buf[2], 4);
      buf[0] = 9;
      strictEqual(ab[1], 9);

      throws(() => Buffer.from(ab.buffer, 6), {
        code: 'ERR_BUFFER_OUT_OF_BOUNDS',
        name: 'RangeError',
        message: '"offset" is outside of buffer bounds',
      });
      throws(() => Buffer.from(ab.buffer, 3, 6), {
        code: 'ERR_BUFFER_OUT_OF_BOUNDS',
        name: 'RangeError',
        message: '"length" is outside of buffer bounds',
      });
    }

    // Test the deprecated Buffer() version also
    {
      const ab = new Uint8Array(5);
      ab[0] = 1;
      ab[1] = 2;
      ab[2] = 3;
      ab[3] = 4;
      ab[4] = 5;
      const buf = Buffer(ab.buffer, 1, 3);
      strictEqual(buf.length, 3);
      strictEqual(buf[0], 2);
      strictEqual(buf[1], 3);
      strictEqual(buf[2], 4);
      buf[0] = 9;
      strictEqual(ab[1], 9);

      throws(() => Buffer(ab.buffer, 6), {
        code: 'ERR_BUFFER_OUT_OF_BOUNDS',
        name: 'RangeError',
        message: '"offset" is outside of buffer bounds',
      });
      throws(() => Buffer(ab.buffer, 3, 6), {
        code: 'ERR_BUFFER_OUT_OF_BOUNDS',
        name: 'RangeError',
        message: '"length" is outside of buffer bounds',
      });
    }

    {
      // If byteOffset is not numeric, it defaults to 0.
      const ab = new ArrayBuffer(10);
      const expected = Buffer.from(ab, 0);
      deepStrictEqual(Buffer.from(ab, 'fhqwhgads'), expected);
      deepStrictEqual(Buffer.from(ab, NaN), expected);
      deepStrictEqual(Buffer.from(ab, {}), expected);
      deepStrictEqual(Buffer.from(ab, []), expected);

      // If byteOffset can be converted to a number, it will be.
      deepStrictEqual(Buffer.from(ab, [1]), Buffer.from(ab, 1));

      // If byteOffset is Infinity, throw.
      throws(
        () => {
          Buffer.from(ab, Infinity);
        },
        {
          code: 'ERR_BUFFER_OUT_OF_BOUNDS',
          name: 'RangeError',
          message: '"offset" is outside of buffer bounds',
        }
      );
    }

    {
      // If length is not numeric, it defaults to 0.
      const ab = new ArrayBuffer(10);
      const expected = Buffer.from(ab, 0, 0);
      deepStrictEqual(Buffer.from(ab, 0, 'fhqwhgads'), expected);
      deepStrictEqual(Buffer.from(ab, 0, NaN), expected);
      deepStrictEqual(Buffer.from(ab, 0, {}), expected);
      deepStrictEqual(Buffer.from(ab, 0, []), expected);

      // If length can be converted to a number, it will be.
      deepStrictEqual(Buffer.from(ab, 0, [1]), Buffer.from(ab, 0, 1));

      // If length is Infinity, throw.
      throws(
        () => {
          Buffer.from(ab, 0, Infinity);
        },
        {
          code: 'ERR_BUFFER_OUT_OF_BOUNDS',
          name: 'RangeError',
          message: '"length" is outside of buffer bounds',
        }
      );
    }

    // Test an array like entry with the length set to NaN.
    deepStrictEqual(Buffer.from({ length: NaN }), Buffer.alloc(0));
  },
};

export const ascii = {
  test(ctrl, env, ctx) {
    // ASCII conversion in node.js simply masks off the high bits,
    // it doesn't do transliteration.
    strictEqual(Buffer.from('hérité').toString('ascii'), 'hC)ritC)');
    // 71 characters, 78 bytes. The ’ character is a triple-byte sequence.
    const input =
      'C’est, graphiquement, la réunion d’un accent aigu ' +
      'et d’un accent grave.';

    const expected =
      'Cb\u0000\u0019est, graphiquement, la rC)union ' +
      'db\u0000\u0019un accent aigu et db\u0000\u0019un ' +
      'accent grave.';

    const buf = Buffer.from(input);

    for (let i = 0; i < expected.length; ++i) {
      strictEqual(buf.slice(i).toString('ascii'), expected.slice(i));

      // Skip remainder of multi-byte sequence.
      if (input.charCodeAt(i) > 65535) ++i;
      if (input.charCodeAt(i) > 127) ++i;
    }
  },
};

export const badHex = {
  test(ctrl, env, ctx) {
    // Test hex strings and bad hex strings
    {
      const buf = Buffer.alloc(4);
      strictEqual(buf.length, 4);
      deepStrictEqual(buf, Buffer.from([0, 0, 0, 0]));
      strictEqual(buf.write('abcdxx', 0, 'hex'), 2);
      deepStrictEqual(buf, Buffer.from([0xab, 0xcd, 0x00, 0x00]));
      strictEqual(buf.toString('hex'), 'abcd0000');
      strictEqual(buf.write('abcdef01', 0, 'hex'), 4);
      deepStrictEqual(buf, Buffer.from([0xab, 0xcd, 0xef, 0x01]));
      strictEqual(buf.toString('hex'), 'abcdef01');

      const copy = Buffer.from(buf.toString('hex'), 'hex');
      strictEqual(buf.toString('hex'), copy.toString('hex'));
    }

    {
      const buf = Buffer.alloc(5);
      strictEqual(buf.write('abcdxx', 1, 'hex'), 2);
      strictEqual(buf.toString('hex'), '00abcd0000');
    }

    {
      const buf = Buffer.alloc(4);
      deepStrictEqual(buf, Buffer.from([0, 0, 0, 0]));
      strictEqual(buf.write('xxabcd', 0, 'hex'), 0);
      deepStrictEqual(buf, Buffer.from([0, 0, 0, 0]));
      strictEqual(buf.write('xxab', 1, 'hex'), 0);
      deepStrictEqual(buf, Buffer.from([0, 0, 0, 0]));
      strictEqual(buf.write('cdxxab', 0, 'hex'), 1);
      deepStrictEqual(buf, Buffer.from([0xcd, 0, 0, 0]));
    }

    {
      const buf = Buffer.alloc(256);
      for (let i = 0; i < 256; i++) buf[i] = i;

      const hex = buf.toString('hex');
      deepStrictEqual(Buffer.from(hex, 'hex'), buf);

      const badHex = `${hex.slice(0, 256)}xx${hex.slice(256, 510)}`;
      deepStrictEqual(Buffer.from(badHex, 'hex'), buf.slice(0, 128));
    }
  },
};

export const bigint64 = {
  test(ctrl, env, ctx) {
    const buf = Buffer.allocUnsafe(8);

    ['LE', 'BE'].forEach(function (endianness) {
      // Should allow simple BigInts to be written and read
      let val = 123456789n;
      buf[`writeBigInt64${endianness}`](val, 0);
      let rtn = buf[`readBigInt64${endianness}`](0);
      strictEqual(val, rtn);

      // Should allow INT64_MAX to be written and read
      val = 0x7fffffffffffffffn;
      buf[`writeBigInt64${endianness}`](val, 0);
      rtn = buf[`readBigInt64${endianness}`](0);
      strictEqual(val, rtn);

      // Should read and write a negative signed 64-bit integer
      val = -123456789n;
      buf[`writeBigInt64${endianness}`](val, 0);
      strictEqual(val, buf[`readBigInt64${endianness}`](0));

      // Should read and write an unsigned 64-bit integer
      val = 123456789n;
      buf[`writeBigUInt64${endianness}`](val, 0);
      strictEqual(val, buf[`readBigUInt64${endianness}`](0));

      // Should throw a RangeError upon INT64_MAX+1 being written
      throws(function () {
        const val = 0x8000000000000000n;
        buf[`writeBigInt64${endianness}`](val, 0);
      }, RangeError);

      // Should throw a RangeError upon UINT64_MAX+1 being written
      throws(
        function () {
          const val = 0x10000000000000000n;
          buf[`writeBigUInt64${endianness}`](val, 0);
        },
        {
          code: 'ERR_OUT_OF_RANGE',
          message:
            'The value of "value" is out of range. It must be ' +
            '>= 0n and < 2n ** 64n. Received 18_446_744_073_709_551_616n',
        }
      );

      // Should throw a TypeError upon invalid input
      throws(function () {
        buf[`writeBigInt64${endianness}`]('bad', 0);
      }, TypeError);

      // Should throw a TypeError upon invalid input
      throws(function () {
        buf[`writeBigUInt64${endianness}`]('bad', 0);
      }, TypeError);
    });
  },
};

export const byteLength = {
  test(ctrl, env, ctx) {
    [[32, 'latin1'], [NaN, 'utf8'], [{}, 'latin1'], []].forEach((args) => {
      throws(() => Buffer.byteLength(...args), {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      });
    });

    ok(ArrayBuffer.isView(new Buffer(10)));
    ok(ArrayBuffer.isView(new SlowBuffer(10)));
    ok(ArrayBuffer.isView(Buffer.alloc(10)));
    ok(ArrayBuffer.isView(Buffer.allocUnsafe(10)));
    ok(ArrayBuffer.isView(Buffer.allocUnsafeSlow(10)));
    ok(ArrayBuffer.isView(Buffer.from('')));

    // buffer
    const incomplete = Buffer.from([0xe4, 0xb8, 0xad, 0xe6, 0x96]);
    strictEqual(Buffer.byteLength(incomplete), 5);
    const ascii = Buffer.from('abc');
    strictEqual(Buffer.byteLength(ascii), 3);

    // ArrayBuffer
    const buffer = new ArrayBuffer(8);
    strictEqual(Buffer.byteLength(buffer), 8);

    // TypedArray
    const int8 = new Int8Array(8);
    strictEqual(Buffer.byteLength(int8), 8);
    const uint8 = new Uint8Array(8);
    strictEqual(Buffer.byteLength(uint8), 8);
    const uintc8 = new Uint8ClampedArray(2);
    strictEqual(Buffer.byteLength(uintc8), 2);
    const int16 = new Int16Array(8);
    strictEqual(Buffer.byteLength(int16), 16);
    const uint16 = new Uint16Array(8);
    strictEqual(Buffer.byteLength(uint16), 16);
    const int32 = new Int32Array(8);
    strictEqual(Buffer.byteLength(int32), 32);
    const uint32 = new Uint32Array(8);
    strictEqual(Buffer.byteLength(uint32), 32);
    const float16 = new Float16Array(8);
    strictEqual(Buffer.byteLength(float16), 16);
    const float32 = new Float32Array(8);
    strictEqual(Buffer.byteLength(float32), 32);
    const float64 = new Float64Array(8);
    strictEqual(Buffer.byteLength(float64), 64);

    // DataView
    const dv = new DataView(new ArrayBuffer(2));
    strictEqual(Buffer.byteLength(dv), 2);

    // Special case: zero length string
    strictEqual(Buffer.byteLength('', 'ascii'), 0);
    strictEqual(Buffer.byteLength('', 'HeX'), 0);

    // utf8
    strictEqual(Buffer.byteLength('∑éllö wørl∂!', 'utf-8'), 19);
    strictEqual(Buffer.byteLength('κλμνξο', 'utf8'), 12);
    strictEqual(Buffer.byteLength('挵挶挷挸挹', 'utf-8'), 15);
    strictEqual(Buffer.byteLength('𠝹𠱓𠱸', 'UTF8'), 12);
    // Without an encoding, utf8 should be assumed
    strictEqual(Buffer.byteLength('hey there'), 9);
    strictEqual(Buffer.byteLength('𠱸挶νξ#xx :)'), 17);
    strictEqual(Buffer.byteLength('hello world', ''), 11);
    // It should also be assumed with unrecognized encoding

    strictEqual(Buffer.byteLength('hello world', 'abc'), 11);
    strictEqual(Buffer.byteLength('ßœ∑≈', 'unkn0wn enc0ding'), 10);

    // base64
    strictEqual(Buffer.byteLength('aGVsbG8gd29ybGQ=', 'base64'), 11);
    strictEqual(Buffer.byteLength('aGVsbG8gd29ybGQ=', 'BASE64'), 11);
    strictEqual(Buffer.byteLength('bm9kZS5qcyByb2NrcyE=', 'base64'), 14);
    strictEqual(Buffer.byteLength('aGkk', 'base64'), 3);
    strictEqual(
      Buffer.byteLength('bHNrZGZsa3NqZmtsc2xrZmFqc2RsZmtqcw==', 'base64'),
      25
    );
    // base64url
    strictEqual(Buffer.byteLength('aGVsbG8gd29ybGQ', 'base64url'), 11);
    strictEqual(Buffer.byteLength('aGVsbG8gd29ybGQ', 'BASE64URL'), 11);
    strictEqual(Buffer.byteLength('bm9kZS5qcyByb2NrcyE', 'base64url'), 14);
    strictEqual(Buffer.byteLength('aGkk', 'base64url'), 3);
    strictEqual(
      Buffer.byteLength('bHNrZGZsa3NqZmtsc2xrZmFqc2RsZmtqcw', 'base64url'),
      25
    );
    // special padding
    strictEqual(Buffer.byteLength('aaa=', 'base64'), 2);
    strictEqual(Buffer.byteLength('aaaa==', 'base64'), 3);
    strictEqual(Buffer.byteLength('aaa=', 'base64url'), 2);
    strictEqual(Buffer.byteLength('aaaa==', 'base64url'), 3);

    strictEqual(Buffer.byteLength('Il était tué'), 14);
    strictEqual(Buffer.byteLength('Il était tué', 'utf8'), 14);

    ['ascii', 'latin1', 'binary']
      .reduce((es, e) => es.concat(e, e.toUpperCase()), [])
      .forEach((encoding) => {
        strictEqual(Buffer.byteLength('Il était tué', encoding), 12);
      });

    ['ucs2', 'ucs-2', 'utf16le', 'utf-16le']
      .reduce((es, e) => es.concat(e, e.toUpperCase()), [])
      .forEach((encoding) => {
        strictEqual(Buffer.byteLength('Il était tué', encoding), 24);
      });

    // Verify that invalid encodings are treated as utf8
    for (let i = 1; i < 10; i++) {
      const encoding = String(i).repeat(i);

      ok(!Buffer.isEncoding(encoding));
      strictEqual(
        Buffer.byteLength('foo', encoding),
        Buffer.byteLength('foo', 'utf8')
      );
    }
  },
};

export const compareOffset = {
  test(ctrl, env, ctx) {
    const a = Buffer.from([1, 2, 3, 4, 5, 6, 7, 8, 9, 0]);
    const b = Buffer.from([5, 6, 7, 8, 9, 0, 1, 2, 3, 4]);

    strictEqual(a.compare(b), -1);

    // Equivalent to a.compare(b).
    strictEqual(a.compare(b, 0), -1);
    throws(() => a.compare(b, '0'), { code: 'ERR_INVALID_ARG_TYPE' });
    strictEqual(a.compare(b, undefined), -1);

    // Equivalent to a.compare(b).
    strictEqual(a.compare(b, 0, undefined, 0), -1);

    // Zero-length target, return 1
    strictEqual(a.compare(b, 0, 0, 0), 1);
    throws(() => a.compare(b, 0, '0', '0'), { code: 'ERR_INVALID_ARG_TYPE' });

    // Equivalent to Buffer.compare(a, b.slice(6, 10))
    strictEqual(a.compare(b, 6, 10), 1);

    // Zero-length source, return -1
    strictEqual(a.compare(b, 6, 10, 0, 0), -1);

    // Zero-length source and target, return 0
    strictEqual(a.compare(b, 0, 0, 0, 0), 0);
    strictEqual(a.compare(b, 1, 1, 2, 2), 0);

    // Equivalent to Buffer.compare(a.slice(4), b.slice(0, 5))
    strictEqual(a.compare(b, 0, 5, 4), 1);

    // Equivalent to Buffer.compare(a.slice(1), b.slice(5))
    strictEqual(a.compare(b, 5, undefined, 1), 1);

    // Equivalent to Buffer.compare(a.slice(2), b.slice(2, 4))
    strictEqual(a.compare(b, 2, 4, 2), -1);

    // Equivalent to Buffer.compare(a.slice(4), b.slice(0, 7))
    strictEqual(a.compare(b, 0, 7, 4), -1);

    // Equivalent to Buffer.compare(a.slice(4, 6), b.slice(0, 7));
    strictEqual(a.compare(b, 0, 7, 4, 6), -1);

    // Null is ambiguous.
    throws(() => a.compare(b, 0, null), { code: 'ERR_INVALID_ARG_TYPE' });

    // Values do not get coerced.
    throws(() => a.compare(b, 0, { valueOf: () => 5 }), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    // Infinity should not be coerced.
    throws(() => a.compare(b, Infinity, -Infinity), {
      code: 'ERR_OUT_OF_RANGE',
    });

    // Zero length target because default for targetEnd <= targetSource
    strictEqual(a.compare(b, 0xff), 1);

    throws(() => a.compare(b, '0xff'), { code: 'ERR_INVALID_ARG_TYPE' });
    throws(() => a.compare(b, 0, '0xff'), { code: 'ERR_INVALID_ARG_TYPE' });

    const oor = { code: 'ERR_OUT_OF_RANGE' };

    throws(() => a.compare(b, 0, 100, 0), oor);
    throws(() => a.compare(b, 0, 1, 0, 100), oor);
    throws(() => a.compare(b, -1), oor);
    throws(() => a.compare(b, 0, Infinity), oor);
    throws(() => a.compare(b, 0, 1, -1), oor);
    throws(() => a.compare(b, -Infinity, Infinity), oor);
    throws(() => a.compare(), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
      message:
        'The "target" argument must be an instance of ' +
        'Buffer or Uint8Array. Received undefined',
    });
  },
};

export const compare = {
  test(ctrl, env, ctx) {
    const b = Buffer.alloc(1, 'a');
    const c = Buffer.alloc(1, 'c');
    const d = Buffer.alloc(2, 'aa');
    const e = new Uint8Array([0x61, 0x61]); // ASCII 'aa', same as d

    strictEqual(b.compare(c), -1);
    strictEqual(c.compare(d), 1);
    strictEqual(d.compare(b), 1);
    strictEqual(d.compare(e), 0);
    strictEqual(b.compare(d), -1);
    strictEqual(b.compare(b), 0);

    strictEqual(Buffer.compare(b, c), -1);
    strictEqual(Buffer.compare(c, d), 1);
    strictEqual(Buffer.compare(d, b), 1);
    strictEqual(Buffer.compare(b, d), -1);
    strictEqual(Buffer.compare(c, c), 0);
    strictEqual(Buffer.compare(e, e), 0);
    strictEqual(Buffer.compare(d, e), 0);
    strictEqual(Buffer.compare(d, b), 1);

    strictEqual(Buffer.compare(Buffer.alloc(0), Buffer.alloc(0)), 0);
    strictEqual(Buffer.compare(Buffer.alloc(0), Buffer.alloc(1)), -1);
    strictEqual(Buffer.compare(Buffer.alloc(1), Buffer.alloc(0)), 1);

    throws(() => Buffer.compare(Buffer.alloc(1), 'abc'), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
    throws(() => Buffer.compare('abc', Buffer.alloc(1)), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    throws(() => Buffer.alloc(1).compare('abc'), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
    });
  },
};

export const concat = {
  test(ctrl, env, ctx) {
    const zero = [];
    const one = [Buffer.from('asdf')];
    const long = [];
    for (let i = 0; i < 10; i++) long.push(Buffer.from('asdf'));

    const flatZero = Buffer.concat(zero);
    const flatOne = Buffer.concat(one);
    const flatLong = Buffer.concat(long);
    const flatLongLen = Buffer.concat(long, 40);

    strictEqual(flatZero.length, 0);
    strictEqual(flatOne.toString(), 'asdf');

    const check = 'asdf'.repeat(10);

    // A special case where concat used to return the first item,
    // if the length is one. This check is to make sure that we don't do that.
    notStrictEqual(flatOne, one[0]);
    strictEqual(flatLong.toString(), check);
    strictEqual(flatLongLen.toString(), check);

    [undefined, null, Buffer.from('hello')].forEach((value) => {
      throws(
        () => {
          Buffer.concat(value);
        },
        {
          name: 'TypeError',
        }
      );
    });

    [[42], ['hello', Buffer.from('world')]].forEach((value) => {
      throws(
        () => {
          Buffer.concat(value);
        },
        {
          name: 'TypeError',
          //code: 'ERR_INVALID_ARG_TYPE',
        }
      );
    });

    throws(
      () => {
        Buffer.concat([Buffer.from('hello'), 3]);
      },
      {
        name: 'TypeError',
        //code: 'ERR_INVALID_ARG_TYPE',
      }
    );

    // eslint-disable-next-line node-core/crypto-check
    const random10 = Buffer.alloc(10);
    crypto.getRandomValues(random10);
    const empty = Buffer.alloc(0);

    notDeepStrictEqual(random10, empty);
    notDeepStrictEqual(random10, Buffer.alloc(10));

    deepStrictEqual(Buffer.concat([], 100), empty);
    deepStrictEqual(Buffer.concat([random10], 0), empty);
    deepStrictEqual(Buffer.concat([random10], 10), random10);
    deepStrictEqual(Buffer.concat([random10, random10], 10), random10);
    deepStrictEqual(Buffer.concat([empty, random10]), random10);
    deepStrictEqual(Buffer.concat([random10, empty, empty]), random10);

    // The tail should be zero-filled
    deepStrictEqual(Buffer.concat([empty], 100), Buffer.alloc(100));
    deepStrictEqual(Buffer.concat([empty], 4096), Buffer.alloc(4096));
    deepStrictEqual(
      Buffer.concat([random10], 40),
      Buffer.concat([random10, Buffer.alloc(30)])
    );

    deepStrictEqual(
      Buffer.concat([
        new Uint8Array([0x41, 0x42]),
        new Uint8Array([0x43, 0x44]),
      ]),
      Buffer.from('ABCD')
    );
  },
};

export const konstants = {
  test(ctrl, env, ctx) {
    strictEqual(typeof MAX_LENGTH, 'number');
    strictEqual(typeof MAX_STRING_LENGTH, 'number');
    ok(MAX_STRING_LENGTH <= MAX_LENGTH);
    throws(
      () => ' '.repeat(MAX_STRING_LENGTH + 1),
      /^RangeError: Invalid string length$/
    );
    ' '.repeat(MAX_STRING_LENGTH); // Should not throw.
    // Legacy values match:
    strictEqual(kMaxLength, MAX_LENGTH);
    strictEqual(kStringMaxLength, MAX_STRING_LENGTH);
  },
};

export const copy = {
  test(ctrl, env, ctx) {
    const b = Buffer.allocUnsafe(1024);
    const c = Buffer.allocUnsafe(512);

    let cntr = 0;

    {
      // copy 512 bytes, from 0 to 512.
      b.fill(++cntr);
      c.fill(++cntr);
      const copied = b.copy(c, 0, 0, 512);
      strictEqual(copied, 512);
      for (let i = 0; i < c.length; i++) {
        strictEqual(c[i], b[i]);
      }
    }

    {
      // Current behavior is to coerce values to integers.
      b.fill(++cntr);
      c.fill(++cntr);
      const copied = b.copy(c, '0', '0', '512');
      strictEqual(copied, 512);
      for (let i = 0; i < c.length; i++) {
        strictEqual(c[i], b[i]);
      }
    }

    {
      // Floats will be converted to integers via `Math.floor`
      b.fill(++cntr);
      c.fill(++cntr);
      const copied = b.copy(c, 0, 0, 512.5);
      strictEqual(copied, 512);
      for (let i = 0; i < c.length; i++) {
        strictEqual(c[i], b[i]);
      }
    }

    {
      // Copy c into b, without specifying sourceEnd
      b.fill(++cntr);
      c.fill(++cntr);
      const copied = c.copy(b, 0, 0);
      strictEqual(copied, c.length);
      for (let i = 0; i < c.length; i++) {
        strictEqual(b[i], c[i]);
      }
    }

    {
      // Copy c into b, without specifying sourceStart
      b.fill(++cntr);
      c.fill(++cntr);
      const copied = c.copy(b, 0);
      strictEqual(copied, c.length);
      for (let i = 0; i < c.length; i++) {
        strictEqual(b[i], c[i]);
      }
    }

    {
      // Copied source range greater than source length
      b.fill(++cntr);
      c.fill(++cntr);
      const copied = c.copy(b, 0, 0, c.length + 1);
      strictEqual(copied, c.length);
      for (let i = 0; i < c.length; i++) {
        strictEqual(b[i], c[i]);
      }
    }

    {
      // Copy longer buffer b to shorter c without targetStart
      b.fill(++cntr);
      c.fill(++cntr);
      const copied = b.copy(c);
      strictEqual(copied, c.length);
      for (let i = 0; i < c.length; i++) {
        strictEqual(c[i], b[i]);
      }
    }

    {
      // Copy starting near end of b to c
      b.fill(++cntr);
      c.fill(++cntr);
      const copied = b.copy(c, 0, b.length - Math.floor(c.length / 2));
      strictEqual(copied, Math.floor(c.length / 2));
      for (let i = 0; i < Math.floor(c.length / 2); i++) {
        strictEqual(c[i], b[b.length - Math.floor(c.length / 2) + i]);
      }
      for (let i = Math.floor(c.length / 2) + 1; i < c.length; i++) {
        strictEqual(c[c.length - 1], c[i]);
      }
    }

    {
      // Try to copy 513 bytes, and check we don't overrun c
      b.fill(++cntr);
      c.fill(++cntr);
      const copied = b.copy(c, 0, 0, 513);
      strictEqual(copied, c.length);
      for (let i = 0; i < c.length; i++) {
        strictEqual(c[i], b[i]);
      }
    }

    {
      // copy 768 bytes from b into b
      b.fill(++cntr);
      b.fill(++cntr, 256);
      const copied = b.copy(b, 0, 256, 1024);
      strictEqual(copied, 768);
      for (let i = 0; i < b.length; i++) {
        strictEqual(b[i], cntr);
      }
    }

    // Copy string longer than buffer length (failure will segfault)
    const bb = Buffer.allocUnsafe(10);
    bb.fill('hello crazy world');

    // Try to copy from before the beginning of b. Should not throw.
    b.copy(c, 0, 100, 10);

    // Throw with invalid source type
    throws(() => Buffer.prototype.copy.call(0), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
    });

    // Copy throws at negative targetStart
    throws(() => Buffer.allocUnsafe(5).copy(Buffer.allocUnsafe(5), -1, 0), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
      message:
        'The value of "targetStart" is out of range. ' +
        'It must be >= 0. Received -1',
    });

    // Copy throws at negative sourceStart
    throws(() => Buffer.allocUnsafe(5).copy(Buffer.allocUnsafe(5), 0, -1), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
      message:
        'The value of "sourceStart" is out of range. ' +
        'It must be >= 0. Received -1',
    });

    {
      // Check sourceEnd resets to targetEnd if former is greater than the latter
      b.fill(++cntr);
      c.fill(++cntr);
      b.copy(c, 0, 0, 1025);
      for (let i = 0; i < c.length; i++) {
        strictEqual(c[i], b[i]);
      }
    }

    // Throw with negative sourceEnd
    throws(() => b.copy(c, 0, 0, -1), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
      message:
        'The value of "sourceEnd" is out of range. ' +
        'It must be >= 0. Received -1',
    });

    // When sourceStart is greater than sourceEnd, zero copied
    strictEqual(b.copy(c, 0, 100, 10), 0);

    // When targetStart > targetLength, zero copied
    strictEqual(b.copy(c, 512, 0, 10), 0);

    // Test that the `target` can be a Uint8Array.
    {
      const d = new Uint8Array(c);
      // copy 512 bytes, from 0 to 512.
      b.fill(++cntr);
      d.fill(++cntr);
      const copied = b.copy(d, 0, 0, 512);
      strictEqual(copied, 512);
      for (let i = 0; i < d.length; i++) {
        strictEqual(d[i], b[i]);
      }
    }

    // Test that the source can be a Uint8Array, too.
    {
      const e = new Uint8Array(b);
      // copy 512 bytes, from 0 to 512.
      e.fill(++cntr);
      c.fill(++cntr);
      const copied = Buffer.prototype.copy.call(e, c, 0, 0, 512);
      strictEqual(copied, 512);
      for (let i = 0; i < c.length; i++) {
        strictEqual(c[i], e[i]);
      }
    }

    // https://github.com/nodejs/node/issues/23668: Do not crash for invalid input.
    c.fill('c');
    b.copy(c, 'not a valid offset');
    // Make sure this acted like a regular copy with `0` offset.
    deepStrictEqual(c, b.slice(0, c.length));

    {
      c.fill('C');
      throws(() => {
        b.copy(c, {
          [Symbol.toPrimitive]() {
            throw new Error('foo');
          },
        });
      }, /foo/);
      // No copying took place:
      deepStrictEqual(c.toString(), 'C'.repeat(c.length));
    }
  },
};

export const equals = {
  test(ctrl, env, ctx) {
    const b = Buffer.from('abcdf');
    const c = Buffer.from('abcdf');
    const d = Buffer.from('abcde');
    const e = Buffer.from('abcdef');

    ok(b.equals(c));
    ok(!c.equals(d));
    ok(!d.equals(e));
    ok(d.equals(d));
    ok(d.equals(new Uint8Array([0x61, 0x62, 0x63, 0x64, 0x65])));

    throws(() => Buffer.alloc(1).equals('abc'), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
    });
  },
};

export const failedAllocTypedArrays = {
  test(ctrl, env, ctx) {
    // Test failed or zero-sized Buffer allocations not affecting typed arrays.
    // This test exists because of a regression that occurred. Because Buffer
    // instances are allocated with the same underlying allocator as TypedArrays,
    // but Buffer's can optional be non-zero filled, there was a regression that
    // occurred when a Buffer allocated failed, the internal flag specifying
    // whether or not to zero-fill was not being reset, causing TypedArrays to
    // allocate incorrectly.
    const zeroArray = new Uint32Array(10).fill(0);
    const sizes = [1e10, 0, 0.1, -1, 'a', undefined, null, NaN];
    const allocators = [
      Buffer,
      SlowBuffer,
      Buffer.alloc,
      Buffer.allocUnsafe,
      Buffer.allocUnsafeSlow,
    ];
    for (const allocator of allocators) {
      for (const size of sizes) {
        try {
          // Some of these allocations are known to fail. If they do,
          // Uint32Array should still produce a zeroed out result.
          allocator(size);
        } catch {
          deepStrictEqual(zeroArray, new Uint32Array(10));
        }
      }
    }
  },
};

export const fakes = {
  test(ctrl, env, ctx) {
    function FakeBuffer() {}
    Object.setPrototypeOf(FakeBuffer, Buffer);
    Object.setPrototypeOf(FakeBuffer.prototype, Buffer.prototype);

    const fb = new FakeBuffer();

    throws(function () {
      Buffer.from(fb);
    }, TypeError);

    throws(function () {
      +Buffer.prototype; // eslint-disable-line no-unused-expressions
    }, TypeError);

    throws(function () {
      Buffer.compare(fb, Buffer.alloc(0));
    }, TypeError);

    throws(function () {
      fb.write('foo');
    }, TypeError);

    throws(function () {
      Buffer.concat([fb, fb]);
    }, TypeError);

    throws(function () {
      fb.toString();
    }, TypeError);

    throws(function () {
      fb.equals(Buffer.alloc(0));
    }, TypeError);

    throws(function () {
      fb.indexOf(5);
    }, TypeError);

    throws(function () {
      fb.readFloatLE(0);
    }, TypeError);

    throws(function () {
      fb.writeFloatLE(0);
    }, TypeError);

    throws(function () {
      fb.fill(0);
    }, TypeError);
  },
};

export const fill = {
  test(ctrl, env, ctx) {
    const SIZE = 28;
    const buf1 = Buffer.allocUnsafe(SIZE);
    const buf2 = Buffer.allocUnsafe(SIZE);

    // Default encoding
    testBufs('abc');
    testBufs('\u0222aa');
    testBufs('a\u0234b\u0235c\u0236');
    testBufs('abc', 4);
    testBufs('abc', 5);
    testBufs('abc', SIZE);
    testBufs('\u0222aa', 2);
    testBufs('\u0222aa', 8);
    testBufs('a\u0234b\u0235c\u0236', 4);
    testBufs('a\u0234b\u0235c\u0236', 12);
    testBufs('abc', 4, 1);
    testBufs('abc', 5, 1);
    testBufs('\u0222aa', 8, 1);
    testBufs('a\u0234b\u0235c\u0236', 4, 1);
    testBufs('a\u0234b\u0235c\u0236', 12, 1);

    // UTF8
    testBufs('abc', 'utf8');
    testBufs('\u0222aa', 'utf8');
    testBufs('a\u0234b\u0235c\u0236', 'utf8');
    testBufs('abc', 4, 'utf8');
    testBufs('abc', 5, 'utf8');
    testBufs('abc', SIZE, 'utf8');
    testBufs('\u0222aa', 2, 'utf8');
    testBufs('\u0222aa', 8, 'utf8');
    testBufs('a\u0234b\u0235c\u0236', 4, 'utf8');
    testBufs('a\u0234b\u0235c\u0236', 12, 'utf8');
    testBufs('abc', 4, 1, 'utf8');
    testBufs('abc', 5, 1, 'utf8');
    testBufs('\u0222aa', 8, 1, 'utf8');
    testBufs('a\u0234b\u0235c\u0236', 4, 1, 'utf8');
    testBufs('a\u0234b\u0235c\u0236', 12, 1, 'utf8');
    strictEqual(Buffer.allocUnsafe(1).fill(0).fill('\u0222')[0], 0xc8);

    // BINARY
    testBufs('abc', 'binary');
    testBufs('\u0222aa', 'binary');
    testBufs('a\u0234b\u0235c\u0236', 'binary');
    testBufs('abc', 4, 'binary');
    testBufs('abc', 5, 'binary');
    testBufs('abc', SIZE, 'binary');
    testBufs('\u0222aa', 2, 'binary');
    testBufs('\u0222aa', 8, 'binary');
    testBufs('a\u0234b\u0235c\u0236', 4, 'binary');
    testBufs('a\u0234b\u0235c\u0236', 12, 'binary');
    testBufs('abc', 4, 1, 'binary');
    testBufs('abc', 5, 1, 'binary');
    testBufs('\u0222aa', 8, 1, 'binary');
    testBufs('a\u0234b\u0235c\u0236', 4, 1, 'binary');
    testBufs('a\u0234b\u0235c\u0236', 12, 1, 'binary');

    // LATIN1
    testBufs('abc', 'latin1');
    testBufs('\u0222aa', 'latin1');
    testBufs('a\u0234b\u0235c\u0236', 'latin1');
    testBufs('abc', 4, 'latin1');
    testBufs('abc', 5, 'latin1');
    testBufs('abc', SIZE, 'latin1');
    testBufs('\u0222aa', 2, 'latin1');
    testBufs('\u0222aa', 8, 'latin1');
    testBufs('a\u0234b\u0235c\u0236', 4, 'latin1');
    testBufs('a\u0234b\u0235c\u0236', 12, 'latin1');
    testBufs('abc', 4, 1, 'latin1');
    testBufs('abc', 5, 1, 'latin1');
    testBufs('\u0222aa', 8, 1, 'latin1');
    testBufs('a\u0234b\u0235c\u0236', 4, 1, 'latin1');
    testBufs('a\u0234b\u0235c\u0236', 12, 1, 'latin1');

    // UCS2
    testBufs('abc', 'ucs2');
    testBufs('\u0222aa', 'ucs2');
    testBufs('a\u0234b\u0235c\u0236', 'ucs2');
    testBufs('abc', 4, 'ucs2');
    testBufs('abc', SIZE, 'ucs2');
    testBufs('\u0222aa', 2, 'ucs2');
    testBufs('\u0222aa', 8, 'ucs2');
    testBufs('a\u0234b\u0235c\u0236', 4, 'ucs2');
    testBufs('a\u0234b\u0235c\u0236', 12, 'ucs2');
    testBufs('abc', 4, 1, 'ucs2');
    testBufs('abc', 5, 1, 'ucs2');
    testBufs('\u0222aa', 8, 1, 'ucs2');
    testBufs('a\u0234b\u0235c\u0236', 4, 1, 'ucs2');
    testBufs('a\u0234b\u0235c\u0236', 12, 1, 'ucs2');
    strictEqual(Buffer.allocUnsafe(1).fill('\u0222', 'ucs2')[0], 0x22);

    // HEX
    testBufs('616263', 'hex');
    testBufs('c8a26161', 'hex');
    testBufs('61c8b462c8b563c8b6', 'hex');
    testBufs('616263', 4, 'hex');
    testBufs('616263', 5, 'hex');
    testBufs('616263', SIZE, 'hex');
    testBufs('c8a26161', 2, 'hex');
    testBufs('c8a26161', 8, 'hex');
    testBufs('61c8b462c8b563c8b6', 4, 'hex');
    testBufs('61c8b462c8b563c8b6', 12, 'hex');
    testBufs('616263', 4, 1, 'hex');
    testBufs('616263', 5, 1, 'hex');
    testBufs('c8a26161', 8, 1, 'hex');
    testBufs('61c8b462c8b563c8b6', 4, 1, 'hex');
    testBufs('61c8b462c8b563c8b6', 12, 1, 'hex');

    throws(
      () => {
        const buf = Buffer.allocUnsafe(SIZE);

        buf.fill('yKJh', 'hex');
      },
      {
        name: 'TypeError',
      }
    );

    throws(
      () => {
        const buf = Buffer.allocUnsafe(SIZE);

        buf.fill('\u0222', 'hex');
      },
      {
        name: 'TypeError',
      }
    );

    // BASE64
    testBufs('YWJj', 'base64');
    testBufs('yKJhYQ==', 'base64');
    testBufs('Yci0Ysi1Y8i2', 'base64');
    testBufs('YWJj', 4, 'base64');
    testBufs('YWJj', SIZE, 'base64');
    testBufs('yKJhYQ==', 2, 'base64');
    testBufs('yKJhYQ==', 8, 'base64');
    testBufs('Yci0Ysi1Y8i2', 4, 'base64');
    testBufs('Yci0Ysi1Y8i2', 12, 'base64');
    testBufs('YWJj', 4, 1, 'base64');
    testBufs('YWJj', 5, 1, 'base64');
    testBufs('yKJhYQ==', 8, 1, 'base64');
    testBufs('Yci0Ysi1Y8i2', 4, 1, 'base64');
    testBufs('Yci0Ysi1Y8i2', 12, 1, 'base64');

    // BASE64URL
    testBufs('YWJj', 'base64url');
    testBufs('yKJhYQ', 'base64url');
    testBufs('Yci0Ysi1Y8i2', 'base64url');
    testBufs('YWJj', 4, 'base64url');
    testBufs('YWJj', SIZE, 'base64url');
    testBufs('yKJhYQ', 2, 'base64url');
    testBufs('yKJhYQ', 8, 'base64url');
    testBufs('Yci0Ysi1Y8i2', 4, 'base64url');
    testBufs('Yci0Ysi1Y8i2', 12, 'base64url');
    testBufs('YWJj', 4, 1, 'base64url');
    testBufs('YWJj', 5, 1, 'base64url');
    testBufs('yKJhYQ', 8, 1, 'base64url');
    testBufs('Yci0Ysi1Y8i2', 4, 1, 'base64url');
    testBufs('Yci0Ysi1Y8i2', 12, 1, 'base64url');

    function deepStrictEqualValues(buf, arr) {
      for (const [index, value] of buf.entries()) {
        deepStrictEqual(value, arr[index]);
      }
    }

    const buf2Fill = Buffer.allocUnsafe(1).fill(2);
    deepStrictEqualValues(genBuffer(4, [buf2Fill]), [2, 2, 2, 2]);
    deepStrictEqualValues(genBuffer(4, [buf2Fill, 1]), [0, 2, 2, 2]);
    deepStrictEqualValues(genBuffer(4, [buf2Fill, 1, 3]), [0, 2, 2, 0]);
    deepStrictEqualValues(genBuffer(4, [buf2Fill, 1, 1]), [0, 0, 0, 0]);
    const hexBufFill = Buffer.allocUnsafe(2).fill(0).fill('0102', 'hex');
    deepStrictEqualValues(genBuffer(4, [hexBufFill]), [1, 2, 1, 2]);
    deepStrictEqualValues(genBuffer(4, [hexBufFill, 1]), [0, 1, 2, 1]);
    deepStrictEqualValues(genBuffer(4, [hexBufFill, 1, 3]), [0, 1, 2, 0]);
    deepStrictEqualValues(genBuffer(4, [hexBufFill, 1, 1]), [0, 0, 0, 0]);

    // Check exceptions
    [
      [0, -1],
      [0, 0, buf1.length + 1],
      ['', -1],
      ['', 0, buf1.length + 1],
      ['', 1, -1],
    ].forEach((args) => {
      throws(() => buf1.fill(...args), { name: 'RangeError' });
    });

    throws(() => buf1.fill('a', 0, buf1.length, 'node rocks!'), {
      code: 'ERR_UNKNOWN_ENCODING',
      name: 'TypeError',
      message: 'Unknown encoding: node rocks!',
    });

    [
      ['a', 0, 0, NaN],
      ['a', 0, 0, false],
    ].forEach((args) => {
      throws(() => buf1.fill(...args), {
        name: 'TypeError',
      });
    });

    throws(() => buf1.fill('a', 0, 0, 'foo'), {
      code: 'ERR_UNKNOWN_ENCODING',
      name: 'TypeError',
      message: 'Unknown encoding: foo',
    });

    function genBuffer(size, args) {
      const b = Buffer.allocUnsafe(size);
      return b.fill(0).fill.apply(b, args);
    }

    function bufReset() {
      buf1.fill(0);
      buf2.fill(0);
    }

    // This is mostly accurate. Except write() won't write partial bytes to the
    // string while fill() blindly copies bytes into memory. To account for that an
    // error will be thrown if not all the data can be written, and the SIZE has
    // been massaged to work with the input characters.
    function writeToFill(string, offset, end, encoding) {
      if (typeof offset === 'string') {
        encoding = offset;
        offset = 0;
        end = buf2.length;
      } else if (typeof end === 'string') {
        encoding = end;
        end = buf2.length;
      } else if (end === undefined) {
        end = buf2.length;
      }

      // Should never be reached.
      if (offset < 0 || end > buf2.length) throw new ERR_OUT_OF_RANGE();

      if (end <= offset) return buf2;

      offset >>>= 0;
      end >>>= 0;
      ok(offset <= buf2.length);

      // Convert "end" to "length" (which write understands).
      const length = end - offset < 0 ? 0 : end - offset;

      let wasZero = false;
      do {
        const written = buf2.write(string, offset, length, encoding);
        offset += written;
        // Safety check in case write falls into infinite loop.
        if (written === 0) {
          if (wasZero) throw new Error('Could not write all data to Buffer');
          else wasZero = true;
        }
      } while (offset < buf2.length);

      return buf2;
    }

    function testBufs(string, offset, length, encoding) {
      bufReset();
      buf1.fill.apply(buf1, arguments);
      // Swap bytes on BE archs for ucs2 encoding.
      deepStrictEqual(
        buf1.fill.apply(buf1, arguments),
        writeToFill.apply(null, arguments)
      );
    }

    // Make sure these throw.
    throws(() => Buffer.allocUnsafe(8).fill('a', -1), {
      code: 'ERR_OUT_OF_RANGE',
    });
    throws(() => Buffer.allocUnsafe(8).fill('a', 0, 9), {
      code: 'ERR_OUT_OF_RANGE',
    });

    // // Make sure this doesn't hang indefinitely.
    Buffer.allocUnsafe(8).fill('');
    Buffer.alloc(8, '');

    {
      const buf = Buffer.alloc(64, 10);
      for (let i = 0; i < buf.length; i++) strictEqual(buf[i], 10);

      buf.fill(11, 0, buf.length >> 1);
      for (let i = 0; i < buf.length >> 1; i++) strictEqual(buf[i], 11);
      for (let i = (buf.length >> 1) + 1; i < buf.length; i++)
        strictEqual(buf[i], 10);

      buf.fill('h');
      for (let i = 0; i < buf.length; i++)
        strictEqual(buf[i], 'h'.charCodeAt(0));

      buf.fill(0);
      for (let i = 0; i < buf.length; i++) strictEqual(buf[i], 0);

      buf.fill(null);
      for (let i = 0; i < buf.length; i++) strictEqual(buf[i], 0);

      buf.fill(1, 16, 32);
      for (let i = 0; i < 16; i++) strictEqual(buf[i], 0);
      for (let i = 16; i < 32; i++) strictEqual(buf[i], 1);
      for (let i = 32; i < buf.length; i++) strictEqual(buf[i], 0);
    }

    {
      const buf = Buffer.alloc(10, 'abc');
      strictEqual(buf.toString(), 'abcabcabca');
      buf.fill('է');
      strictEqual(buf.toString(), 'էէէէէ');
    }

    // Make sure "end" is properly checked, even if it's magically mangled using
    // Symbol.toPrimitive.
    {
      throws(
        () => {
          const end = {
            [Symbol.toPrimitive]() {
              return 1;
            },
          };
          Buffer.alloc(1).fill(Buffer.alloc(1), 0, end);
        },
        {
          code: 'ERR_INVALID_ARG_TYPE',
          message:
            'The "end" argument must be of type number. Received an ' +
            'instance of Object',
        }
      );
    }

    // Test that bypassing 'length' won't cause an abort.
    // Node.js throws an error in this case because it's often the case that the
    // Buffer might contain uninitialized memory and we need to prevent overreads.
    // However, our implementation is backed entirely by ArrayBuffer/Uint8Array
    // and always has initialized memory, so if the user does something funky
    // like this, they'll get back undefineds.
    {
      const buf = Buffer.from('w00t');
      Object.defineProperty(buf, 'length', {
        value: 1337,
        enumerable: true,
      });
      buf.fill('');
    }

    deepStrictEqual(
      Buffer.allocUnsafeSlow(16).fill('ab', 'utf16le'),
      Buffer.from('61006200610062006100620061006200', 'hex')
    );

    deepStrictEqual(
      Buffer.allocUnsafeSlow(15).fill('ab', 'utf16le'),
      Buffer.from('610062006100620061006200610062', 'hex')
    );

    deepStrictEqual(
      Buffer.allocUnsafeSlow(16).fill('ab', 'utf16le'),
      Buffer.from('61006200610062006100620061006200', 'hex')
    );
    deepStrictEqual(
      Buffer.allocUnsafeSlow(16).fill('a', 'utf16le'),
      Buffer.from('61006100610061006100610061006100', 'hex')
    );

    strictEqual(
      Buffer.allocUnsafeSlow(16).fill('a', 'utf16le').toString('utf16le'),
      'a'.repeat(8)
    );
    strictEqual(
      Buffer.allocUnsafeSlow(16).fill('a', 'latin1').toString('latin1'),
      'a'.repeat(16)
    );
    strictEqual(
      Buffer.allocUnsafeSlow(16).fill('a', 'utf8').toString('utf8'),
      'a'.repeat(16)
    );

    strictEqual(
      Buffer.allocUnsafeSlow(16).fill('Љ', 'utf16le').toString('utf16le'),
      'Љ'.repeat(8)
    );
    strictEqual(
      Buffer.allocUnsafeSlow(16).fill('Љ', 'latin1').toString('latin1'),
      '\t'.repeat(16)
    );
    strictEqual(
      Buffer.allocUnsafeSlow(16).fill('Љ', 'utf8').toString('utf8'),
      'Љ'.repeat(8)
    );

    throws(
      () => {
        const buf = Buffer.from('a'.repeat(1000));

        buf.fill('This is not correctly encoded', 'hex');
      },
      {
        name: 'TypeError',
      }
    );

    {
      const bufEmptyString = Buffer.alloc(5, '');
      strictEqual(bufEmptyString.toString(), '\x00\x00\x00\x00\x00');

      const bufEmptyArray = Buffer.alloc(5, []);
      strictEqual(bufEmptyArray.toString(), '\x00\x00\x00\x00\x00');

      const bufEmptyBuffer = Buffer.alloc(5, Buffer.alloc(5));
      strictEqual(bufEmptyBuffer.toString(), '\x00\x00\x00\x00\x00');

      const bufZero = Buffer.alloc(5, 0);
      strictEqual(bufZero.toString(), '\x00\x00\x00\x00\x00');
    }
  },
};

export const includes = {
  test(ctrl, env, ctx) {
    const b = Buffer.from('abcdef');
    const buf_a = Buffer.from('a');
    const buf_bc = Buffer.from('bc');
    const buf_f = Buffer.from('f');
    const buf_z = Buffer.from('z');
    const buf_empty = Buffer.from('');

    ok(b.includes('a'));
    ok(!b.includes('a', 1));
    ok(!b.includes('a', -1));
    ok(!b.includes('a', -4));
    ok(b.includes('a', -b.length));
    ok(b.includes('a', NaN));
    ok(b.includes('a', -Infinity));
    ok(!b.includes('a', Infinity));
    ok(b.includes('bc'));
    ok(!b.includes('bc', 2));
    ok(!b.includes('bc', -1));
    ok(!b.includes('bc', -3));
    ok(b.includes('bc', -5));
    ok(b.includes('bc', NaN));
    ok(b.includes('bc', -Infinity));
    ok(!b.includes('bc', Infinity));
    ok(b.includes('f'), b.length - 1);
    ok(!b.includes('z'));
    ok(b.includes(''));
    ok(b.includes('', 1));
    ok(b.includes('', b.length + 1));
    ok(b.includes('', Infinity));
    ok(b.includes(buf_a));
    ok(!b.includes(buf_a, 1));
    ok(!b.includes(buf_a, -1));
    ok(!b.includes(buf_a, -4));
    ok(b.includes(buf_a, -b.length));
    ok(b.includes(buf_a, NaN));
    ok(b.includes(buf_a, -Infinity));
    ok(!b.includes(buf_a, Infinity));
    ok(b.includes(buf_bc));
    ok(!b.includes(buf_bc, 2));
    ok(!b.includes(buf_bc, -1));
    ok(!b.includes(buf_bc, -3));
    ok(b.includes(buf_bc, -5));
    ok(b.includes(buf_bc, NaN));
    ok(b.includes(buf_bc, -Infinity));
    ok(!b.includes(buf_bc, Infinity));
    ok(b.includes(buf_f), b.length - 1);
    ok(!b.includes(buf_z));
    ok(b.includes(buf_empty));
    ok(b.includes(buf_empty, 1));
    ok(b.includes(buf_empty, b.length + 1));
    ok(b.includes(buf_empty, Infinity));
    ok(b.includes(0x61));
    ok(!b.includes(0x61, 1));
    ok(!b.includes(0x61, -1));
    ok(!b.includes(0x61, -4));
    ok(b.includes(0x61, -b.length));
    ok(b.includes(0x61, NaN));
    ok(b.includes(0x61, -Infinity));
    ok(!b.includes(0x61, Infinity));
    ok(!b.includes(0x0));

    // test offsets
    ok(b.includes('d', 2));
    ok(b.includes('f', 5));
    ok(b.includes('f', -1));
    ok(!b.includes('f', 6));

    ok(b.includes(Buffer.from('d'), 2));
    ok(b.includes(Buffer.from('f'), 5));
    ok(b.includes(Buffer.from('f'), -1));
    ok(!b.includes(Buffer.from('f'), 6));

    ok(!Buffer.from('ff').includes(Buffer.from('f'), 1, 'ucs2'));

    // test hex encoding
    strictEqual(
      Buffer.from(b.toString('hex'), 'hex').includes('64', 0, 'hex'),
      true
    );
    strictEqual(
      Buffer.from(b.toString('hex'), 'hex').includes(
        Buffer.from('64', 'hex'),
        0,
        'hex'
      ),
      true
    );

    // Test base64 encoding
    strictEqual(
      Buffer.from(b.toString('base64'), 'base64').includes('ZA==', 0, 'base64'),
      true
    );
    strictEqual(
      Buffer.from(b.toString('base64'), 'base64').includes(
        Buffer.from('ZA==', 'base64'),
        0,
        'base64'
      ),
      true
    );

    // test ascii encoding
    strictEqual(
      Buffer.from(b.toString('ascii'), 'ascii').includes('d', 0, 'ascii'),
      true
    );
    strictEqual(
      Buffer.from(b.toString('ascii'), 'ascii').includes(
        Buffer.from('d', 'ascii'),
        0,
        'ascii'
      ),
      true
    );

    // Test latin1 encoding
    strictEqual(
      Buffer.from(b.toString('latin1'), 'latin1').includes('d', 0, 'latin1'),
      true
    );
    strictEqual(
      Buffer.from(b.toString('latin1'), 'latin1').includes(
        Buffer.from('d', 'latin1'),
        0,
        'latin1'
      ),
      true
    );

    // Test binary encoding
    strictEqual(
      Buffer.from(b.toString('binary'), 'binary').includes('d', 0, 'binary'),
      true
    );
    strictEqual(
      Buffer.from(b.toString('binary'), 'binary').includes(
        Buffer.from('d', 'binary'),
        0,
        'binary'
      ),
      true
    );

    // test ucs2 encoding
    let twoByteString = Buffer.from('\u039a\u0391\u03a3\u03a3\u0395', 'ucs2');

    ok(twoByteString.includes('\u0395', 4, 'ucs2'));
    ok(twoByteString.includes('\u03a3', -4, 'ucs2'));
    ok(twoByteString.includes('\u03a3', -6, 'ucs2'));
    ok(twoByteString.includes(Buffer.from('\u03a3', 'ucs2'), -6, 'ucs2'));
    ok(!twoByteString.includes('\u03a3', -2, 'ucs2'));

    const mixedByteStringUcs2 = Buffer.from(
      '\u039a\u0391abc\u03a3\u03a3\u0395',
      'ucs2'
    );
    ok(mixedByteStringUcs2.includes('bc', 0, 'ucs2'));
    ok(mixedByteStringUcs2.includes('\u03a3', 0, 'ucs2'));
    ok(!mixedByteStringUcs2.includes('\u0396', 0, 'ucs2'));

    ok(mixedByteStringUcs2.includes(Buffer.from('bc', 'ucs2'), 0, 'ucs2'));
    ok(mixedByteStringUcs2.includes(Buffer.from('\u03a3', 'ucs2'), 0, 'ucs2'));
    ok(!mixedByteStringUcs2.includes(Buffer.from('\u0396', 'ucs2'), 0, 'ucs2'));

    twoByteString = Buffer.from('\u039a\u0391\u03a3\u03a3\u0395', 'ucs2');

    // Test single char pattern
    ok(twoByteString.includes('\u039a', 0, 'ucs2'));
    ok(twoByteString.includes('\u0391', 0, 'ucs2'), 'Alpha');
    ok(twoByteString.includes('\u03a3', 0, 'ucs2'), 'First Sigma');
    ok(twoByteString.includes('\u03a3', 6, 'ucs2'), 'Second Sigma');
    ok(twoByteString.includes('\u0395', 0, 'ucs2'), 'Epsilon');
    ok(!twoByteString.includes('\u0392', 0, 'ucs2'), 'Not beta');

    // Test multi-char pattern
    ok(twoByteString.includes('\u039a\u0391', 0, 'ucs2'), 'Lambda Alpha');
    ok(twoByteString.includes('\u0391\u03a3', 0, 'ucs2'), 'Alpha Sigma');
    ok(twoByteString.includes('\u03a3\u03a3', 0, 'ucs2'), 'Sigma Sigma');
    ok(twoByteString.includes('\u03a3\u0395', 0, 'ucs2'), 'Sigma Epsilon');

    const mixedByteStringUtf8 = Buffer.from(
      '\u039a\u0391abc\u03a3\u03a3\u0395'
    );
    ok(mixedByteStringUtf8.includes('bc'));
    ok(mixedByteStringUtf8.includes('bc', 5));
    ok(mixedByteStringUtf8.includes('bc', -8));
    ok(mixedByteStringUtf8.includes('\u03a3'));
    ok(!mixedByteStringUtf8.includes('\u0396'));

    // Test complex string includes algorithms. Only trigger for long strings.
    // Long string that isn't a simple repeat of a shorter string.
    let longString = 'A';
    for (let i = 66; i < 76; i++) {
      // from 'B' to 'K'
      longString = longString + String.fromCharCode(i) + longString;
    }

    const longBufferString = Buffer.from(longString);

    // Pattern of 15 chars, repeated every 16 chars in long
    let pattern = 'ABACABADABACABA';
    for (let i = 0; i < longBufferString.length - pattern.length; i += 7) {
      const includes = longBufferString.includes(pattern, i);
      ok(includes, `Long ABACABA...-string at index ${i}`);
    }
    ok(longBufferString.includes('AJABACA'), 'Long AJABACA, First J');
    ok(longBufferString.includes('AJABACA', 511), 'Long AJABACA, Second J');

    pattern = 'JABACABADABACABA';
    ok(longBufferString.includes(pattern), 'Long JABACABA..., First J');
    ok(longBufferString.includes(pattern, 512), 'Long JABACABA..., Second J');

    // Search for a non-ASCII string in a pure ASCII string.
    const asciiString = Buffer.from(
      'arglebargleglopglyfarglebargleglopglyfarglebargleglopglyf'
    );
    ok(!asciiString.includes('\x2061'));
    ok(asciiString.includes('leb', 0));

    // Search in string containing many non-ASCII chars.
    const allCodePoints = [];
    for (let i = 0; i < 65534; i++) allCodePoints[i] = i;
    const allCharsString =
      String.fromCharCode.apply(String, allCodePoints) +
      String.fromCharCode(65534, 65535);
    const allCharsBufferUtf8 = Buffer.from(allCharsString);
    const allCharsBufferUcs2 = Buffer.from(allCharsString, 'ucs2');

    // Search for string long enough to trigger complex search with ASCII pattern
    // and UC16 subject.
    ok(!allCharsBufferUtf8.includes('notfound'));
    ok(!allCharsBufferUcs2.includes('notfound'));

    // Find substrings in Utf8.
    let lengths = [1, 3, 15]; // Single char, simple and complex.
    let indices = [0x5, 0x60, 0x400, 0x680, 0x7ee, 0xff02, 0x16610, 0x2f77b];
    for (let lengthIndex = 0; lengthIndex < lengths.length; lengthIndex++) {
      for (let i = 0; i < indices.length; i++) {
        const index = indices[i];
        let length = lengths[lengthIndex];

        if (index + length > 0x7f) {
          length = 2 * length;
        }

        if (index + length > 0x7ff) {
          length = 3 * length;
        }

        if (index + length > 0xffff) {
          length = 4 * length;
        }

        const patternBufferUtf8 = allCharsBufferUtf8.slice(
          index,
          index + length
        );
        ok(index, allCharsBufferUtf8.includes(patternBufferUtf8));

        const patternStringUtf8 = patternBufferUtf8.toString();
        ok(index, allCharsBufferUtf8.includes(patternStringUtf8));
      }
    }

    // Find substrings in Usc2.
    lengths = [2, 4, 16]; // Single char, simple and complex.
    indices = [0x5, 0x65, 0x105, 0x205, 0x285, 0x2005, 0x2085, 0xfff0];
    for (let lengthIndex = 0; lengthIndex < lengths.length; lengthIndex++) {
      for (let i = 0; i < indices.length; i++) {
        const index = indices[i] * 2;
        const length = lengths[lengthIndex];

        const patternBufferUcs2 = allCharsBufferUcs2.slice(
          index,
          index + length
        );
        ok(allCharsBufferUcs2.includes(patternBufferUcs2, 0, 'ucs2'));

        const patternStringUcs2 = patternBufferUcs2.toString('ucs2');
        ok(allCharsBufferUcs2.includes(patternStringUcs2, 0, 'ucs2'));
      }
    }

    [() => {}, {}, []].forEach((val) => {
      throws(() => b.includes(val), {
        name: 'TypeError',
      });
    });

    // Test truncation of Number arguments to uint8
    {
      const buf = Buffer.from('this is a test');
      ok(buf.includes(0x6973));
      ok(buf.includes(0x697320));
      ok(buf.includes(0x69732069));
      ok(buf.includes(0x697374657374));
      ok(buf.includes(0x69737374));
      ok(buf.includes(0x69737465));
      ok(buf.includes(0x69737465));
      ok(buf.includes(-140));
      ok(buf.includes(-152));
      ok(!buf.includes(0xff));
      ok(!buf.includes(0xffff));
    }
  },
};

export const indexof = {
  test(ctrl, env, ctx) {
    const b = Buffer.from('abcdef');
    const buf_a = Buffer.from('a');
    const buf_bc = Buffer.from('bc');
    const buf_f = Buffer.from('f');
    const buf_z = Buffer.from('z');
    const buf_empty = Buffer.from('');

    const s = 'abcdef';

    strictEqual(b.indexOf('a'), 0);
    strictEqual(b.indexOf('a', 1), -1);
    strictEqual(b.indexOf('a', -1), -1);
    strictEqual(b.indexOf('a', -4), -1);
    strictEqual(b.indexOf('a', -b.length), 0);
    strictEqual(b.indexOf('a', NaN), 0);
    strictEqual(b.indexOf('a', -Infinity), 0);
    strictEqual(b.indexOf('a', Infinity), -1);
    strictEqual(b.indexOf('bc'), 1);
    strictEqual(b.indexOf('bc', 2), -1);
    strictEqual(b.indexOf('bc', -1), -1);
    strictEqual(b.indexOf('bc', -3), -1);
    strictEqual(b.indexOf('bc', -5), 1);
    strictEqual(b.indexOf('bc', NaN), 1);
    strictEqual(b.indexOf('bc', -Infinity), 1);
    strictEqual(b.indexOf('bc', Infinity), -1);
    strictEqual(b.indexOf('f'), b.length - 1);
    strictEqual(b.indexOf('z'), -1);
    strictEqual(b.indexOf(''), 0);
    strictEqual(b.indexOf('', 1), 1);
    strictEqual(b.indexOf('', b.length + 1), b.length);
    strictEqual(b.indexOf('', Infinity), b.length);
    strictEqual(b.indexOf(buf_a), 0);
    strictEqual(b.indexOf(buf_a, 1), -1);
    strictEqual(b.indexOf(buf_a, -1), -1);
    strictEqual(b.indexOf(buf_a, -4), -1);
    strictEqual(b.indexOf(buf_a, -b.length), 0);
    strictEqual(b.indexOf(buf_a, NaN), 0);
    strictEqual(b.indexOf(buf_a, -Infinity), 0);
    strictEqual(b.indexOf(buf_a, Infinity), -1);
    strictEqual(b.indexOf(buf_bc), 1);
    strictEqual(b.indexOf(buf_bc, 2), -1);
    strictEqual(b.indexOf(buf_bc, -1), -1);
    strictEqual(b.indexOf(buf_bc, -3), -1);
    strictEqual(b.indexOf(buf_bc, -5), 1);
    strictEqual(b.indexOf(buf_bc, NaN), 1);
    strictEqual(b.indexOf(buf_bc, -Infinity), 1);
    strictEqual(b.indexOf(buf_bc, Infinity), -1);
    strictEqual(b.indexOf(buf_f), b.length - 1);
    strictEqual(b.indexOf(buf_z), -1);
    strictEqual(b.indexOf(buf_empty), 0);
    strictEqual(b.indexOf(buf_empty, 1), 1);
    strictEqual(b.indexOf(buf_empty, b.length + 1), b.length);
    strictEqual(b.indexOf(buf_empty, Infinity), b.length);
    strictEqual(b.indexOf(0x61), 0);
    strictEqual(b.indexOf(0x61, 1), -1);
    strictEqual(b.indexOf(0x61, -1), -1);
    strictEqual(b.indexOf(0x61, -4), -1);
    strictEqual(b.indexOf(0x61, -b.length), 0);
    strictEqual(b.indexOf(0x61, NaN), 0);
    strictEqual(b.indexOf(0x61, -Infinity), 0);
    strictEqual(b.indexOf(0x61, Infinity), -1);
    strictEqual(b.indexOf(0x0), -1);

    // test offsets
    strictEqual(b.indexOf('d', 2), 3);
    strictEqual(b.indexOf('f', 5), 5);
    strictEqual(b.indexOf('f', -1), 5);
    strictEqual(b.indexOf('f', 6), -1);

    strictEqual(b.indexOf(Buffer.from('d'), 2), 3);
    strictEqual(b.indexOf(Buffer.from('f'), 5), 5);
    strictEqual(b.indexOf(Buffer.from('f'), -1), 5);
    strictEqual(b.indexOf(Buffer.from('f'), 6), -1);

    strictEqual(Buffer.from('ff').indexOf(Buffer.from('f'), 1, 'ucs2'), -1);

    // Test invalid and uppercase encoding
    strictEqual(b.indexOf('b', 'utf8'), 1);
    strictEqual(b.indexOf('b', 'UTF8'), 1);
    strictEqual(b.indexOf('62', 'HEX'), 1);

    throws(() => b.indexOf('bad', 'enc'), /Unknown encoding: enc/);

    // test hex encoding
    strictEqual(
      Buffer.from(b.toString('hex'), 'hex').indexOf('64', 0, 'hex'),
      3
    );
    strictEqual(
      Buffer.from(b.toString('hex'), 'hex').indexOf(
        Buffer.from('64', 'hex'),
        0,
        'hex'
      ),
      3
    );

    // Test base64 encoding
    strictEqual(
      Buffer.from(b.toString('base64'), 'base64').indexOf('ZA==', 0, 'base64'),
      3
    );
    strictEqual(
      Buffer.from(b.toString('base64'), 'base64').indexOf(
        Buffer.from('ZA==', 'base64'),
        0,
        'base64'
      ),
      3
    );

    // Test base64url encoding
    strictEqual(
      Buffer.from(b.toString('base64url'), 'base64url').indexOf(
        'ZA==',
        0,
        'base64url'
      ),
      3
    );

    // test ascii encoding
    strictEqual(
      Buffer.from(b.toString('ascii'), 'ascii').indexOf('d', 0, 'ascii'),
      3
    );
    strictEqual(
      Buffer.from(b.toString('ascii'), 'ascii').indexOf(
        Buffer.from('d', 'ascii'),
        0,
        'ascii'
      ),
      3
    );

    // Test latin1 encoding
    strictEqual(
      Buffer.from(b.toString('latin1'), 'latin1').indexOf('d', 0, 'latin1'),
      3
    );
    strictEqual(
      Buffer.from(b.toString('latin1'), 'latin1').indexOf(
        Buffer.from('d', 'latin1'),
        0,
        'latin1'
      ),
      3
    );
    strictEqual(
      Buffer.from('aa\u00e8aa', 'latin1').indexOf('\u00e8', 'latin1'),
      2
    );
    strictEqual(Buffer.from('\u00e8', 'latin1').indexOf('\u00e8', 'latin1'), 0);
    strictEqual(
      Buffer.from('\u00e8', 'latin1').indexOf(
        Buffer.from('\u00e8', 'latin1'),
        'latin1'
      ),
      0
    );

    // Test binary encoding
    strictEqual(
      Buffer.from(b.toString('binary'), 'binary').indexOf('d', 0, 'binary'),
      3
    );
    strictEqual(
      Buffer.from(b.toString('binary'), 'binary').indexOf(
        Buffer.from('d', 'binary'),
        0,
        'binary'
      ),
      3
    );
    strictEqual(
      Buffer.from('aa\u00e8aa', 'binary').indexOf('\u00e8', 'binary'),
      2
    );
    strictEqual(Buffer.from('\u00e8', 'binary').indexOf('\u00e8', 'binary'), 0);
    strictEqual(
      Buffer.from('\u00e8', 'binary').indexOf(
        Buffer.from('\u00e8', 'binary'),
        'binary'
      ),
      0
    );

    // Test optional offset with passed encoding
    strictEqual(Buffer.from('aaaa0').indexOf('30', 'hex'), 4);
    strictEqual(Buffer.from('aaaa00a').indexOf('3030', 'hex'), 4);

    {
      // Test usc2 and utf16le encoding
      ['ucs2', 'utf16le'].forEach((encoding) => {
        const twoByteString = Buffer.from(
          '\u039a\u0391\u03a3\u03a3\u0395',
          encoding
        );

        strictEqual(twoByteString.indexOf('\u0395', 4, encoding), 8);
        strictEqual(twoByteString.indexOf('\u03a3', -4, encoding), 6);
        strictEqual(twoByteString.indexOf('\u03a3', -6, encoding), 4);
        strictEqual(
          twoByteString.indexOf(Buffer.from('\u03a3', encoding), -6, encoding),
          4
        );
        strictEqual(-1, twoByteString.indexOf('\u03a3', -2, encoding));
      });
    }

    const mixedByteStringUcs2 = Buffer.from(
      '\u039a\u0391abc\u03a3\u03a3\u0395',
      'ucs2'
    );
    strictEqual(mixedByteStringUcs2.indexOf('bc', 0, 'ucs2'), 6);
    strictEqual(mixedByteStringUcs2.indexOf('\u03a3', 0, 'ucs2'), 10);
    strictEqual(-1, mixedByteStringUcs2.indexOf('\u0396', 0, 'ucs2'));

    strictEqual(
      mixedByteStringUcs2.indexOf(Buffer.from('bc', 'ucs2'), 0, 'ucs2'),
      6
    );
    strictEqual(
      mixedByteStringUcs2.indexOf(Buffer.from('\u03a3', 'ucs2'), 0, 'ucs2'),
      10
    );
    strictEqual(
      -1,
      mixedByteStringUcs2.indexOf(Buffer.from('\u0396', 'ucs2'), 0, 'ucs2')
    );

    {
      const twoByteString = Buffer.from(
        '\u039a\u0391\u03a3\u03a3\u0395',
        'ucs2'
      );

      // Test single char pattern
      strictEqual(twoByteString.indexOf('\u039a', 0, 'ucs2'), 0);
      let index = twoByteString.indexOf('\u0391', 0, 'ucs2');
      strictEqual(index, 2, `Alpha - at index ${index}`);
      index = twoByteString.indexOf('\u03a3', 0, 'ucs2');
      strictEqual(index, 4, `First Sigma - at index ${index}`);
      index = twoByteString.indexOf('\u03a3', 6, 'ucs2');
      strictEqual(index, 6, `Second Sigma - at index ${index}`);
      index = twoByteString.indexOf('\u0395', 0, 'ucs2');
      strictEqual(index, 8, `Epsilon - at index ${index}`);
      index = twoByteString.indexOf('\u0392', 0, 'ucs2');
      strictEqual(-1, index, `Not beta - at index ${index}`);

      // Test multi-char pattern
      index = twoByteString.indexOf('\u039a\u0391', 0, 'ucs2');
      strictEqual(index, 0, `Lambda Alpha - at index ${index}`);
      index = twoByteString.indexOf('\u0391\u03a3', 0, 'ucs2');
      strictEqual(index, 2, `Alpha Sigma - at index ${index}`);
      index = twoByteString.indexOf('\u03a3\u03a3', 0, 'ucs2');
      strictEqual(index, 4, `Sigma Sigma - at index ${index}`);
      index = twoByteString.indexOf('\u03a3\u0395', 0, 'ucs2');
      strictEqual(index, 6, `Sigma Epsilon - at index ${index}`);
    }

    const mixedByteStringUtf8 = Buffer.from(
      '\u039a\u0391abc\u03a3\u03a3\u0395'
    );
    strictEqual(mixedByteStringUtf8.indexOf('bc'), 5);
    strictEqual(mixedByteStringUtf8.indexOf('bc', 5), 5);
    strictEqual(mixedByteStringUtf8.indexOf('bc', -8), 5);
    strictEqual(mixedByteStringUtf8.indexOf('\u03a3'), 7);
    strictEqual(mixedByteStringUtf8.indexOf('\u0396'), -1);

    // Test complex string indexOf algorithms. Only trigger for long strings.
    // Long string that isn't a simple repeat of a shorter string.
    let longString = 'A';
    for (let i = 66; i < 76; i++) {
      // from 'B' to 'K'
      longString = longString + String.fromCharCode(i) + longString;
    }

    const longBufferString = Buffer.from(longString);

    // Pattern of 15 chars, repeated every 16 chars in long
    let pattern = 'ABACABADABACABA';
    for (let i = 0; i < longBufferString.length - pattern.length; i += 7) {
      const index = longBufferString.indexOf(pattern, i);
      strictEqual(
        (i + 15) & ~0xf,
        index,
        `Long ABACABA...-string at index ${i}`
      );
    }

    let index = longBufferString.indexOf('AJABACA');
    strictEqual(index, 510, `Long AJABACA, First J - at index ${index}`);
    index = longBufferString.indexOf('AJABACA', 511);
    strictEqual(index, 1534, `Long AJABACA, Second J - at index ${index}`);

    pattern = 'JABACABADABACABA';
    index = longBufferString.indexOf(pattern);
    strictEqual(index, 511, `Long JABACABA..., First J - at index ${index}`);
    index = longBufferString.indexOf(pattern, 512);
    strictEqual(index, 1535, `Long JABACABA..., Second J - at index ${index}`);

    // Search for a non-ASCII string in a pure ASCII string.
    const asciiString = Buffer.from(
      'arglebargleglopglyfarglebargleglopglyfarglebargleglopglyf'
    );
    strictEqual(-1, asciiString.indexOf('\x2061'));
    strictEqual(asciiString.indexOf('leb', 0), 3);

    // Search in string containing many non-ASCII chars.
    const allCodePoints = [];
    for (let i = 0; i < 65534; i++) allCodePoints[i] = i;
    const allCharsString =
      String.fromCharCode.apply(String, allCodePoints) +
      String.fromCharCode(65534, 65535);
    const allCharsBufferUtf8 = Buffer.from(allCharsString);
    const allCharsBufferUcs2 = Buffer.from(allCharsString, 'ucs2');

    // Search for string long enough to trigger complex search with ASCII pattern
    // and UC16 subject.
    strictEqual(-1, allCharsBufferUtf8.indexOf('notfound'));
    strictEqual(-1, allCharsBufferUcs2.indexOf('notfound'));

    // Needle is longer than haystack, but only because it's encoded as UTF-16
    strictEqual(Buffer.from('aaaa').indexOf('a'.repeat(4), 'ucs2'), -1);

    strictEqual(Buffer.from('aaaa').indexOf('a'.repeat(4), 'utf8'), 0);
    strictEqual(Buffer.from('aaaa').indexOf('你好', 'ucs2'), -1);

    // Haystack has odd length, but the needle is UCS2.
    strictEqual(Buffer.from('aaaaa').indexOf('b', 'ucs2'), -1);

    {
      // Find substrings in Utf8.
      const lengths = [1, 3, 15]; // Single char, simple and complex.
      const indices = [
        0x5, 0x60, 0x400, 0x680, 0x7ee, 0xff02, 0x16610, 0x2f77b,
      ];
      for (let lengthIndex = 0; lengthIndex < lengths.length; lengthIndex++) {
        for (let i = 0; i < indices.length; i++) {
          const index = indices[i];
          let length = lengths[lengthIndex];

          if (index + length > 0x7f) {
            length = 2 * length;
          }

          if (index + length > 0x7ff) {
            length = 3 * length;
          }

          if (index + length > 0xffff) {
            length = 4 * length;
          }

          const patternBufferUtf8 = allCharsBufferUtf8.slice(
            index,
            index + length
          );
          strictEqual(index, allCharsBufferUtf8.indexOf(patternBufferUtf8));

          const patternStringUtf8 = patternBufferUtf8.toString();
          strictEqual(index, allCharsBufferUtf8.indexOf(patternStringUtf8));
        }
      }
    }

    {
      // Find substrings in Usc2.
      const lengths = [2, 4, 16]; // Single char, simple and complex.
      const indices = [0x5, 0x65, 0x105, 0x205, 0x285, 0x2005, 0x2085, 0xfff0];
      for (let lengthIndex = 0; lengthIndex < lengths.length; lengthIndex++) {
        for (let i = 0; i < indices.length; i++) {
          const index = indices[i] * 2;
          const length = lengths[lengthIndex];

          const patternBufferUcs2 = allCharsBufferUcs2.slice(
            index,
            index + length
          );
          strictEqual(
            index,
            allCharsBufferUcs2.indexOf(patternBufferUcs2, 0, 'ucs2')
          );

          const patternStringUcs2 = patternBufferUcs2.toString('ucs2');
          strictEqual(
            index,
            allCharsBufferUcs2.indexOf(patternStringUcs2, 0, 'ucs2')
          );
        }
      }
    }

    [() => {}, {}, []].forEach((val) => {
      throws(() => b.indexOf(val), {
        name: 'TypeError',
      });
    });

    // Test weird offset arguments.
    // The following offsets coerce to NaN or 0, searching the whole Buffer
    strictEqual(b.indexOf('b', undefined), 1);
    strictEqual(b.indexOf('b', {}), 1);
    strictEqual(b.indexOf('b', 0), 1);
    strictEqual(b.indexOf('b', null), 1);
    strictEqual(b.indexOf('b', []), 1);

    // The following offset coerces to 2, in other words +[2] === 2
    strictEqual(b.indexOf('b', [2]), -1);

    // Behavior should match String.indexOf()
    strictEqual(b.indexOf('b', undefined), s.indexOf('b', undefined));
    strictEqual(b.indexOf('b', {}), s.indexOf('b', {}));
    strictEqual(b.indexOf('b', 0), s.indexOf('b', 0));
    strictEqual(b.indexOf('b', null), s.indexOf('b', null));
    strictEqual(b.indexOf('b', []), s.indexOf('b', []));
    strictEqual(b.indexOf('b', [2]), s.indexOf('b', [2]));

    // All code for handling encodings is shared between Buffer.indexOf and
    // Buffer.lastIndexOf, so only testing the separate lastIndexOf semantics.

    // Test lastIndexOf basic functionality; Buffer b contains 'abcdef'.
    // lastIndexOf string:
    strictEqual(b.lastIndexOf('a'), 0);
    strictEqual(b.lastIndexOf('a', 1), 0);
    strictEqual(b.lastIndexOf('b', 1), 1);
    strictEqual(b.lastIndexOf('c', 1), -1);
    strictEqual(b.lastIndexOf('a', -1), 0);
    strictEqual(b.lastIndexOf('a', -4), 0);
    strictEqual(b.lastIndexOf('a', -b.length), 0);
    strictEqual(b.lastIndexOf('a', -b.length - 1), -1);
    strictEqual(b.lastIndexOf('a', NaN), 0);
    strictEqual(b.lastIndexOf('a', -Infinity), -1);
    strictEqual(b.lastIndexOf('a', Infinity), 0);
    // lastIndexOf Buffer:
    strictEqual(b.lastIndexOf(buf_a), 0);
    strictEqual(b.lastIndexOf(buf_a, 1), 0);
    strictEqual(b.lastIndexOf(buf_a, -1), 0);
    strictEqual(b.lastIndexOf(buf_a, -4), 0);
    strictEqual(b.lastIndexOf(buf_a, -b.length), 0);
    strictEqual(b.lastIndexOf(buf_a, -b.length - 1), -1);
    strictEqual(b.lastIndexOf(buf_a, NaN), 0);
    strictEqual(b.lastIndexOf(buf_a, -Infinity), -1);
    strictEqual(b.lastIndexOf(buf_a, Infinity), 0);
    strictEqual(b.lastIndexOf(buf_bc), 1);
    strictEqual(b.lastIndexOf(buf_bc, 2), 1);
    strictEqual(b.lastIndexOf(buf_bc, -1), 1);
    strictEqual(b.lastIndexOf(buf_bc, -3), 1);
    strictEqual(b.lastIndexOf(buf_bc, -5), 1);
    strictEqual(b.lastIndexOf(buf_bc, -6), -1);
    strictEqual(b.lastIndexOf(buf_bc, NaN), 1);
    strictEqual(b.lastIndexOf(buf_bc, -Infinity), -1);
    strictEqual(b.lastIndexOf(buf_bc, Infinity), 1);
    strictEqual(b.lastIndexOf(buf_f), b.length - 1);
    strictEqual(b.lastIndexOf(buf_z), -1);
    strictEqual(b.lastIndexOf(buf_empty), b.length);
    strictEqual(b.lastIndexOf(buf_empty, 1), 1);
    strictEqual(b.lastIndexOf(buf_empty, b.length + 1), b.length);
    strictEqual(b.lastIndexOf(buf_empty, Infinity), b.length);
    // lastIndexOf number:
    strictEqual(b.lastIndexOf(0x61), 0);
    strictEqual(b.lastIndexOf(0x61, 1), 0);
    strictEqual(b.lastIndexOf(0x61, -1), 0);
    strictEqual(b.lastIndexOf(0x61, -4), 0);
    strictEqual(b.lastIndexOf(0x61, -b.length), 0);
    strictEqual(b.lastIndexOf(0x61, -b.length - 1), -1);
    strictEqual(b.lastIndexOf(0x61, NaN), 0);
    strictEqual(b.lastIndexOf(0x61, -Infinity), -1);
    strictEqual(b.lastIndexOf(0x61, Infinity), 0);
    strictEqual(b.lastIndexOf(0x0), -1);

    // Test weird offset arguments.
    // The following offsets coerce to NaN, searching the whole Buffer
    strictEqual(b.lastIndexOf('b', undefined), 1);
    strictEqual(b.lastIndexOf('b', {}), 1);

    // The following offsets coerce to 0
    strictEqual(b.lastIndexOf('b', 0), -1);
    strictEqual(b.lastIndexOf('b', null), -1);
    strictEqual(b.lastIndexOf('b', []), -1);

    // The following offset coerces to 2, in other words +[2] === 2
    strictEqual(b.lastIndexOf('b', [2]), 1);

    // Behavior should match String.lastIndexOf()
    strictEqual(b.lastIndexOf('b', undefined), s.lastIndexOf('b', undefined));
    strictEqual(b.lastIndexOf('b', {}), s.lastIndexOf('b', {}));
    strictEqual(b.lastIndexOf('b', 0), s.lastIndexOf('b', 0));
    strictEqual(b.lastIndexOf('b', null), s.lastIndexOf('b', null));
    strictEqual(b.lastIndexOf('b', []), s.lastIndexOf('b', []));
    strictEqual(b.lastIndexOf('b', [2]), s.lastIndexOf('b', [2]));

    // Test needles longer than the haystack.
    strictEqual(b.lastIndexOf('aaaaaaaaaaaaaaa', 'ucs2'), -1);
    strictEqual(b.lastIndexOf('aaaaaaaaaaaaaaa', 'utf8'), -1);
    strictEqual(b.lastIndexOf('aaaaaaaaaaaaaaa', 'latin1'), -1);
    strictEqual(b.lastIndexOf('aaaaaaaaaaaaaaa', 'binary'), -1);
    strictEqual(b.lastIndexOf(Buffer.from('aaaaaaaaaaaaaaa')), -1);
    strictEqual(b.lastIndexOf('aaaaaaaaaaaaaaa', 2, 'ucs2'), -1);
    strictEqual(b.lastIndexOf('aaaaaaaaaaaaaaa', 3, 'utf8'), -1);
    strictEqual(b.lastIndexOf('aaaaaaaaaaaaaaa', 5, 'latin1'), -1);
    strictEqual(b.lastIndexOf('aaaaaaaaaaaaaaa', 5, 'binary'), -1);
    strictEqual(b.lastIndexOf(Buffer.from('aaaaaaaaaaaaaaa'), 7), -1);

    // 你好 expands to a total of 6 bytes using UTF-8 and 4 bytes using UTF-16
    strictEqual(buf_bc.lastIndexOf('你好', 'ucs2'), -1);
    strictEqual(buf_bc.lastIndexOf('你好', 'utf8'), -1);
    strictEqual(buf_bc.lastIndexOf('你好', 'latin1'), -1);
    strictEqual(buf_bc.lastIndexOf('你好', 'binary'), -1);
    strictEqual(buf_bc.lastIndexOf(Buffer.from('你好')), -1);
    strictEqual(buf_bc.lastIndexOf('你好', 2, 'ucs2'), -1);
    strictEqual(buf_bc.lastIndexOf('你好', 3, 'utf8'), -1);
    strictEqual(buf_bc.lastIndexOf('你好', 5, 'latin1'), -1);
    strictEqual(buf_bc.lastIndexOf('你好', 5, 'binary'), -1);
    strictEqual(buf_bc.lastIndexOf(Buffer.from('你好'), 7), -1);

    // Test lastIndexOf on a longer buffer:
    const bufferString = Buffer.from('a man a plan a canal panama');
    strictEqual(bufferString.lastIndexOf('canal'), 15);
    strictEqual(bufferString.lastIndexOf('panama'), 21);
    strictEqual(bufferString.lastIndexOf('a man a plan a canal panama'), 0);
    strictEqual(-1, bufferString.lastIndexOf('a man a plan a canal mexico'));
    strictEqual(
      -1,
      bufferString.lastIndexOf('a man a plan a canal mexico city')
    );
    strictEqual(-1, bufferString.lastIndexOf(Buffer.from('a'.repeat(1000))));
    strictEqual(bufferString.lastIndexOf('a man a plan', 4), 0);
    strictEqual(bufferString.lastIndexOf('a '), 13);
    strictEqual(bufferString.lastIndexOf('a ', 13), 13);
    strictEqual(bufferString.lastIndexOf('a ', 12), 6);
    strictEqual(bufferString.lastIndexOf('a ', 5), 0);
    strictEqual(bufferString.lastIndexOf('a ', -1), 13);
    strictEqual(bufferString.lastIndexOf('a ', -27), 0);
    strictEqual(-1, bufferString.lastIndexOf('a ', -28));

    // Test lastIndexOf for the case that the first character can be found,
    // but in a part of the buffer that does not make search to search
    // due do length constraints.
    const abInUCS2 = Buffer.from('ab', 'ucs2');
    strictEqual(-1, Buffer.from('µaaaa¶bbbb', 'latin1').lastIndexOf('µ'));
    strictEqual(-1, Buffer.from('µaaaa¶bbbb', 'binary').lastIndexOf('µ'));
    strictEqual(-1, Buffer.from('bc').lastIndexOf('ab'));
    strictEqual(-1, Buffer.from('abc').lastIndexOf('qa'));
    strictEqual(-1, Buffer.from('abcdef').lastIndexOf('qabc'));
    strictEqual(-1, Buffer.from('bc').lastIndexOf(Buffer.from('ab')));
    strictEqual(-1, Buffer.from('bc', 'ucs2').lastIndexOf('ab', 'ucs2'));
    strictEqual(-1, Buffer.from('bc', 'ucs2').lastIndexOf(abInUCS2));

    strictEqual(Buffer.from('abc').lastIndexOf('ab'), 0);
    strictEqual(Buffer.from('abc').lastIndexOf('ab', 1), 0);
    strictEqual(Buffer.from('abc').lastIndexOf('ab', 2), 0);
    strictEqual(Buffer.from('abc').lastIndexOf('ab', 3), 0);

    // The above tests test the LINEAR and SINGLE-CHAR strategies.
    // Now, we test the BOYER-MOORE-HORSPOOL strategy.
    // Test lastIndexOf on a long buffer w multiple matches:
    // pattern = 'JABACABADABACABA';
    strictEqual(longBufferString.lastIndexOf(pattern), 1535);
    strictEqual(longBufferString.lastIndexOf(pattern, 1535), 1535);
    strictEqual(longBufferString.lastIndexOf(pattern, 1534), 511);

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
    pattern = reallyLong.slice(0, 100000); // First 1/50th of the pattern.
    strictEqual(reallyLong.lastIndexOf(pattern), 4751360);
    strictEqual(reallyLong.lastIndexOf(pattern, 4000000), 3932160);
    strictEqual(reallyLong.lastIndexOf(pattern, 3000000), 2949120);
    pattern = reallyLong.slice(100000, 200000); // Second 1/50th.
    strictEqual(reallyLong.lastIndexOf(pattern), 4728480);
    pattern = reallyLong.slice(0, 1000000); // First 1/5th.
    strictEqual(reallyLong.lastIndexOf(pattern), 3932160);
    pattern = reallyLong.slice(0, 2000000); // first 2/5ths.
    strictEqual(reallyLong.lastIndexOf(pattern), 0);

    // Test truncation of Number arguments to uint8
    {
      const buf = Buffer.from('this is a test');
      strictEqual(buf.indexOf(0x6973), 3);
      strictEqual(buf.indexOf(0x697320), 4);
      strictEqual(buf.indexOf(0x69732069), 2);
      strictEqual(buf.indexOf(0x697374657374), 0);
      strictEqual(buf.indexOf(0x69737374), 0);
      strictEqual(buf.indexOf(0x69737465), 11);
      strictEqual(buf.indexOf(0x69737465), 11);
      strictEqual(buf.indexOf(-140), 0);
      strictEqual(buf.indexOf(-152), 1);
      strictEqual(buf.indexOf(0xff), -1);
      strictEqual(buf.indexOf(0xffff), -1);
    }

    // Test that Uint8Array arguments are okay.
    {
      const needle = new Uint8Array([0x66, 0x6f, 0x6f]);
      const haystack = Buffer.from('a foo b foo');
      strictEqual(haystack.indexOf(needle), 2);
      strictEqual(haystack.lastIndexOf(needle), haystack.length - 3);
    }
  },
};

export const inheritance = {
  test(ctrl, env, ctx) {
    function T(n) {
      const ui8 = new Uint8Array(n);
      Object.setPrototypeOf(ui8, T.prototype);
      return ui8;
    }
    Object.setPrototypeOf(T.prototype, Buffer.prototype);
    Object.setPrototypeOf(T, Buffer);

    T.prototype.sum = function sum() {
      let cntr = 0;
      for (let i = 0; i < this.length; i++) cntr += this[i];
      return cntr;
    };

    const vals = [new T(4), T(4)];

    vals.forEach(function (t) {
      strictEqual(t.constructor, T);
      strictEqual(Object.getPrototypeOf(t), T.prototype);
      strictEqual(
        Object.getPrototypeOf(Object.getPrototypeOf(t)),
        Buffer.prototype
      );

      t.fill(5);
      let cntr = 0;
      for (let i = 0; i < t.length; i++) cntr += t[i];
      strictEqual(cntr, t.length * 5);

      // Check this does not throw
      t.toString();
    });
  },
};

export const iterator = {
  test(ctrl, env, ctx) {
    const buffer = Buffer.from([1, 2, 3, 4, 5]);
    let arr;
    let b;

    // Buffers should be iterable

    arr = [];

    for (b of buffer) arr.push(b);

    deepStrictEqual(arr, [1, 2, 3, 4, 5]);

    // Buffer iterators should be iterable

    arr = [];

    for (b of buffer[Symbol.iterator]()) arr.push(b);

    deepStrictEqual(arr, [1, 2, 3, 4, 5]);

    // buffer#values() should return iterator for values

    arr = [];

    for (b of buffer.values()) arr.push(b);

    deepStrictEqual(arr, [1, 2, 3, 4, 5]);

    // buffer#keys() should return iterator for keys

    arr = [];

    for (b of buffer.keys()) arr.push(b);

    deepStrictEqual(arr, [0, 1, 2, 3, 4]);

    // buffer#entries() should return iterator for entries

    arr = [];

    for (b of buffer.entries()) arr.push(b);

    deepStrictEqual(arr, [
      [0, 1],
      [1, 2],
      [2, 3],
      [3, 4],
      [4, 5],
    ]);
  },
};

export const negativeAlloc = {
  test(ctrl, env, ctx) {
    const msg = {
      name: 'RangeError',
    };

    // Test that negative Buffer length inputs throw errors.

    throws(() => Buffer(-100), msg);
    throws(() => Buffer(-1), msg);
    throws(() => Buffer(NaN), msg);

    throws(() => Buffer.alloc(-100), msg);
    throws(() => Buffer.alloc(-1), msg);
    throws(() => Buffer.alloc(NaN), msg);

    throws(() => Buffer.allocUnsafe(-100), msg);
    throws(() => Buffer.allocUnsafe(-1), msg);
    throws(() => Buffer.allocUnsafe(NaN), msg);

    throws(() => Buffer.allocUnsafeSlow(-100), msg);
    throws(() => Buffer.allocUnsafeSlow(-1), msg);
    throws(() => Buffer.allocUnsafeSlow(NaN), msg);

    throws(() => SlowBuffer(-100), msg);
    throws(() => SlowBuffer(-1), msg);
    throws(() => SlowBuffer(NaN), msg);
  },
};

export const overMaxLength = {
  test(ctrl, env, ctx) {
    const bufferMaxSizeMsg = {
      name: 'RangeError',
    };

    throws(() => Buffer((-1 >>> 0) + 2), bufferMaxSizeMsg);
    throws(() => SlowBuffer((-1 >>> 0) + 2), bufferMaxSizeMsg);
    throws(() => Buffer.alloc((-1 >>> 0) + 2), bufferMaxSizeMsg);
    throws(() => Buffer.allocUnsafe((-1 >>> 0) + 2), bufferMaxSizeMsg);
    throws(() => Buffer.allocUnsafeSlow((-1 >>> 0) + 2), bufferMaxSizeMsg);

    throws(() => Buffer(kMaxLength + 1), bufferMaxSizeMsg);
    throws(() => SlowBuffer(kMaxLength + 1), bufferMaxSizeMsg);
    throws(() => Buffer.alloc(kMaxLength + 1), bufferMaxSizeMsg);
    throws(() => Buffer.allocUnsafe(kMaxLength + 1), bufferMaxSizeMsg);
    throws(() => Buffer.allocUnsafeSlow(kMaxLength + 1), bufferMaxSizeMsg);

    // issue GH-4331
    throws(() => Buffer.allocUnsafe(0x100000001), bufferMaxSizeMsg);
    throws(() => Buffer.allocUnsafe(0xfffffffff), bufferMaxSizeMsg);
  },
};

export const read = {
  test(ctrl, env, ctx) {
    // Testing basic buffer read functions
    const buf = Buffer.from([
      0xa4, 0xfd, 0x48, 0xea, 0xcf, 0xff, 0xd9, 0x01, 0xde,
    ]);

    function read(buff, funx, args, expected) {
      strictEqual(buff[funx](...args), expected);
      throws(() => buff[funx](-1, args[1]), { code: 'ERR_OUT_OF_RANGE' });
    }

    // Testing basic functionality of readDoubleBE() and readDoubleLE()
    read(buf, 'readDoubleBE', [1], -3.1827727774563287e295);
    read(buf, 'readDoubleLE', [1], -6.966010051009108e144);

    // Testing basic functionality of readFloatBE() and readFloatLE()
    read(buf, 'readFloatBE', [1], -1.6691549692541768e37);
    read(buf, 'readFloatLE', [1], -7861303808);

    // Testing basic functionality of readInt8()
    read(buf, 'readInt8', [1], -3);

    // Testing basic functionality of readInt16BE() and readInt16LE()
    read(buf, 'readInt16BE', [1], -696);
    read(buf, 'readInt16LE', [1], 0x48fd);

    // Testing basic functionality of readInt32BE() and readInt32LE()
    read(buf, 'readInt32BE', [1], -45552945);
    read(buf, 'readInt32LE', [1], -806729475);

    // Testing basic functionality of readIntBE() and readIntLE()
    read(buf, 'readIntBE', [1, 1], -3);
    read(buf, 'readIntLE', [2, 1], 0x48);

    // Testing basic functionality of readUInt8()
    read(buf, 'readUInt8', [1], 0xfd);

    // Testing basic functionality of readUInt16BE() and readUInt16LE()
    read(buf, 'readUInt16BE', [2], 0x48ea);
    read(buf, 'readUInt16LE', [2], 0xea48);

    // Testing basic functionality of readUInt32BE() and readUInt32LE()
    read(buf, 'readUInt32BE', [1], 0xfd48eacf);
    read(buf, 'readUInt32LE', [1], 0xcfea48fd);

    // Testing basic functionality of readUIntBE() and readUIntLE()
    read(buf, 'readUIntBE', [2, 2], 0x48ea);
    read(buf, 'readUIntLE', [2, 2], 0xea48);

    // Error name and message
    const OOR_ERROR = {
      name: 'RangeError',
    };

    const OOB_ERROR = {
      name: 'RangeError',
      message: 'Attempt to access memory outside buffer bounds',
    };

    // Attempt to overflow buffers, similar to previous bug in array buffers
    throws(() => Buffer.allocUnsafe(8).readFloatBE(0xffffffff), OOR_ERROR);

    throws(() => Buffer.allocUnsafe(8).readFloatLE(0xffffffff), OOR_ERROR);

    // Ensure negative values can't get past offset
    throws(() => Buffer.allocUnsafe(8).readFloatBE(-1), OOR_ERROR);
    throws(() => Buffer.allocUnsafe(8).readFloatLE(-1), OOR_ERROR);

    // Offset checks
    {
      const buf = Buffer.allocUnsafe(0);

      throws(() => buf.readUInt8(0), OOB_ERROR);
      throws(() => buf.readInt8(0), OOB_ERROR);
    }

    [16, 32].forEach((bit) => {
      const buf = Buffer.allocUnsafe(bit / 8 - 1);
      [`Int${bit}B`, `Int${bit}L`, `UInt${bit}B`, `UInt${bit}L`].forEach(
        (fn) => {
          throws(() => buf[`read${fn}E`](0), OOB_ERROR);
        }
      );
    });

    [16, 32].forEach((bits) => {
      const buf = Buffer.from([0xff, 0xff, 0xff, 0xff]);
      ['LE', 'BE'].forEach((endian) => {
        strictEqual(
          buf[`readUInt${bits}${endian}`](0),
          0xffffffff >>> (32 - bits)
        );

        strictEqual(
          buf[`readInt${bits}${endian}`](0),
          0xffffffff >> (32 - bits)
        );
      });
    });
  },
};

export const readDouble = {
  test(ctrl, env, ctx) {
    // Test (64 bit) double
    const buffer = Buffer.allocUnsafe(8);

    buffer[0] = 0x55;
    buffer[1] = 0x55;
    buffer[2] = 0x55;
    buffer[3] = 0x55;
    buffer[4] = 0x55;
    buffer[5] = 0x55;
    buffer[6] = 0xd5;
    buffer[7] = 0x3f;
    strictEqual(buffer.readDoubleBE(0), 1.1945305291680097e103);
    strictEqual(buffer.readDoubleLE(0), 0.3333333333333333);

    buffer[0] = 1;
    buffer[1] = 0;
    buffer[2] = 0;
    buffer[3] = 0;
    buffer[4] = 0;
    buffer[5] = 0;
    buffer[6] = 0xf0;
    buffer[7] = 0x3f;
    strictEqual(buffer.readDoubleBE(0), 7.291122019655968e-304);
    strictEqual(buffer.readDoubleLE(0), 1.0000000000000002);

    buffer[0] = 2;
    strictEqual(buffer.readDoubleBE(0), 4.778309726801735e-299);
    strictEqual(buffer.readDoubleLE(0), 1.0000000000000004);

    buffer[0] = 1;
    buffer[6] = 0;
    buffer[7] = 0;
    // eslint-disable-next-line no-loss-of-precision
    strictEqual(buffer.readDoubleBE(0), 7.291122019556398e-304);
    strictEqual(buffer.readDoubleLE(0), 5e-324);

    buffer[0] = 0xff;
    buffer[1] = 0xff;
    buffer[2] = 0xff;
    buffer[3] = 0xff;
    buffer[4] = 0xff;
    buffer[5] = 0xff;
    buffer[6] = 0x0f;
    buffer[7] = 0x00;
    ok(Number.isNaN(buffer.readDoubleBE(0)));
    strictEqual(buffer.readDoubleLE(0), 2.225073858507201e-308);

    buffer[6] = 0xef;
    buffer[7] = 0x7f;
    ok(Number.isNaN(buffer.readDoubleBE(0)));
    strictEqual(buffer.readDoubleLE(0), 1.7976931348623157e308);

    buffer[0] = 0;
    buffer[1] = 0;
    buffer[2] = 0;
    buffer[3] = 0;
    buffer[4] = 0;
    buffer[5] = 0;
    buffer[6] = 0xf0;
    buffer[7] = 0x3f;
    strictEqual(buffer.readDoubleBE(0), 3.03865e-319);
    strictEqual(buffer.readDoubleLE(0), 1);

    buffer[6] = 0;
    buffer[7] = 0x40;
    strictEqual(buffer.readDoubleBE(0), 3.16e-322);
    strictEqual(buffer.readDoubleLE(0), 2);

    buffer[7] = 0xc0;
    strictEqual(buffer.readDoubleBE(0), 9.5e-322);
    strictEqual(buffer.readDoubleLE(0), -2);

    buffer[6] = 0x10;
    buffer[7] = 0;
    strictEqual(buffer.readDoubleBE(0), 2.0237e-320);
    strictEqual(buffer.readDoubleLE(0), 2.2250738585072014e-308);

    buffer[6] = 0;
    strictEqual(buffer.readDoubleBE(0), 0);
    strictEqual(buffer.readDoubleLE(0), 0);
    ok(1 / buffer.readDoubleLE(0) >= 0);

    buffer[7] = 0x80;
    strictEqual(buffer.readDoubleBE(0), 6.3e-322);
    strictEqual(buffer.readDoubleLE(0), -0);
    ok(1 / buffer.readDoubleLE(0) < 0);

    buffer[6] = 0xf0;
    buffer[7] = 0x7f;
    strictEqual(buffer.readDoubleBE(0), 3.0418e-319);
    strictEqual(buffer.readDoubleLE(0), Infinity);

    buffer[7] = 0xff;
    strictEqual(buffer.readDoubleBE(0), 3.04814e-319);
    strictEqual(buffer.readDoubleLE(0), -Infinity);

    ['readDoubleLE', 'readDoubleBE'].forEach((fn) => {
      // Verify that default offset works fine.
      buffer[fn](undefined);
      buffer[fn]();

      ['', '0', null, {}, [], () => {}, true, false].forEach((off) => {
        throws(() => buffer[fn](off), { code: 'ERR_INVALID_ARG_TYPE' });
      });

      [Infinity, -1, 1].forEach((offset) => {
        throws(() => buffer[fn](offset), {
          code: 'ERR_OUT_OF_RANGE',
          name: 'RangeError',
        });
      });

      throws(() => Buffer.alloc(1)[fn](1), {
        code: 'ERR_BUFFER_OUT_OF_BOUNDS',
        name: 'RangeError',
        message: 'Attempt to access memory outside buffer bounds',
      });

      [NaN, 1.01].forEach((offset) => {
        throws(() => buffer[fn](offset), {
          code: 'ERR_OUT_OF_RANGE',
          name: 'RangeError',
          message:
            'The value of "offset" is out of range. ' +
            `It must be an integer. Received ${offset}`,
        });
      });
    });
  },
};

export const readFloat = {
  test(ctrl, env, ctx) {
    // Test 32 bit float
    const buffer = Buffer.alloc(4);

    buffer[0] = 0;
    buffer[1] = 0;
    buffer[2] = 0x80;
    buffer[3] = 0x3f;
    strictEqual(buffer.readFloatBE(0), 4.600602988224807e-41);
    strictEqual(buffer.readFloatLE(0), 1);

    buffer[0] = 0;
    buffer[1] = 0;
    buffer[2] = 0;
    buffer[3] = 0xc0;
    strictEqual(buffer.readFloatBE(0), 2.6904930515036488e-43);
    strictEqual(buffer.readFloatLE(0), -2);

    buffer[0] = 0xff;
    buffer[1] = 0xff;
    buffer[2] = 0x7f;
    buffer[3] = 0x7f;
    ok(Number.isNaN(buffer.readFloatBE(0)));
    strictEqual(buffer.readFloatLE(0), 3.4028234663852886e38);

    buffer[0] = 0xab;
    buffer[1] = 0xaa;
    buffer[2] = 0xaa;
    buffer[3] = 0x3e;
    strictEqual(buffer.readFloatBE(0), -1.2126478207002966e-12);
    strictEqual(buffer.readFloatLE(0), 0.3333333432674408);

    buffer[0] = 0;
    buffer[1] = 0;
    buffer[2] = 0;
    buffer[3] = 0;
    strictEqual(buffer.readFloatBE(0), 0);
    strictEqual(buffer.readFloatLE(0), 0);
    ok(1 / buffer.readFloatLE(0) >= 0);

    buffer[3] = 0x80;
    strictEqual(buffer.readFloatBE(0), 1.793662034335766e-43);
    strictEqual(buffer.readFloatLE(0), -0);
    ok(1 / buffer.readFloatLE(0) < 0);

    buffer[0] = 0;
    buffer[1] = 0;
    buffer[2] = 0x80;
    buffer[3] = 0x7f;
    strictEqual(buffer.readFloatBE(0), 4.609571298396486e-41);
    strictEqual(buffer.readFloatLE(0), Infinity);

    buffer[0] = 0;
    buffer[1] = 0;
    buffer[2] = 0x80;
    buffer[3] = 0xff;
    strictEqual(buffer.readFloatBE(0), 4.627507918739843e-41);
    strictEqual(buffer.readFloatLE(0), -Infinity);

    ['readFloatLE', 'readFloatBE'].forEach((fn) => {
      // Verify that default offset works fine.
      buffer[fn](undefined);
      buffer[fn]();

      ['', '0', null, {}, [], () => {}, true, false].forEach((off) => {
        throws(() => buffer[fn](off), { code: 'ERR_INVALID_ARG_TYPE' });
      });

      [Infinity, -1, 1].forEach((offset) => {
        throws(() => buffer[fn](offset), {
          code: 'ERR_OUT_OF_RANGE',
          name: 'RangeError',
        });
      });

      throws(() => Buffer.alloc(1)[fn](1), {
        code: 'ERR_BUFFER_OUT_OF_BOUNDS',
        name: 'RangeError',
        message: 'Attempt to access memory outside buffer bounds',
      });

      [NaN, 1.01].forEach((offset) => {
        throws(() => buffer[fn](offset), {
          code: 'ERR_OUT_OF_RANGE',
          name: 'RangeError',
          message:
            'The value of "offset" is out of range. ' +
            `It must be an integer. Received ${offset}`,
        });
      });
    });
  },
};

export const readInt = {
  test(ctrl, env, ctx) {
    // Test OOB
    {
      const buffer = Buffer.alloc(4);

      ['Int8', 'Int16BE', 'Int16LE', 'Int32BE', 'Int32LE'].forEach((fn) => {
        // Verify that default offset works fine.
        buffer[`read${fn}`](undefined);
        buffer[`read${fn}`]();

        ['', '0', null, {}, [], () => {}, true, false].forEach((o) => {
          throws(() => buffer[`read${fn}`](o), {
            code: 'ERR_INVALID_ARG_TYPE',
            name: 'TypeError',
          });
        });

        [Infinity, -1, -4294967295].forEach((offset) => {
          throws(() => buffer[`read${fn}`](offset), {
            code: 'ERR_OUT_OF_RANGE',
            name: 'RangeError',
          });
        });

        [NaN, 1.01].forEach((offset) => {
          throws(() => buffer[`read${fn}`](offset), {
            code: 'ERR_OUT_OF_RANGE',
            name: 'RangeError',
          });
        });
      });
    }

    // Test 8 bit signed integers
    {
      const data = Buffer.from([0x23, 0xab, 0x7c, 0xef]);

      strictEqual(data.readInt8(0), 0x23);

      data[0] = 0xff;
      strictEqual(data.readInt8(0), -1);

      data[0] = 0x87;
      strictEqual(data.readInt8(0), -121);
      strictEqual(data.readInt8(1), -85);
      strictEqual(data.readInt8(2), 124);
      strictEqual(data.readInt8(3), -17);
    }

    // Test 16 bit integers
    {
      const buffer = Buffer.from([0x16, 0x79, 0x65, 0x6e, 0x69, 0x78]);

      strictEqual(buffer.readInt16BE(0), 0x1679);
      strictEqual(buffer.readInt16LE(0), 0x7916);

      buffer[0] = 0xff;
      buffer[1] = 0x80;
      strictEqual(buffer.readInt16BE(0), -128);
      strictEqual(buffer.readInt16LE(0), -32513);

      buffer[0] = 0x77;
      buffer[1] = 0x65;
      strictEqual(buffer.readInt16BE(0), 0x7765);
      strictEqual(buffer.readInt16BE(1), 0x6565);
      strictEqual(buffer.readInt16BE(2), 0x656e);
      strictEqual(buffer.readInt16BE(3), 0x6e69);
      strictEqual(buffer.readInt16BE(4), 0x6978);
      strictEqual(buffer.readInt16LE(0), 0x6577);
      strictEqual(buffer.readInt16LE(1), 0x6565);
      strictEqual(buffer.readInt16LE(2), 0x6e65);
      strictEqual(buffer.readInt16LE(3), 0x696e);
      strictEqual(buffer.readInt16LE(4), 0x7869);
    }

    // Test 32 bit integers
    {
      const buffer = Buffer.from([0x43, 0x53, 0x16, 0x79, 0x36, 0x17]);

      strictEqual(buffer.readInt32BE(0), 0x43531679);
      strictEqual(buffer.readInt32LE(0), 0x79165343);

      buffer[0] = 0xff;
      buffer[1] = 0xfe;
      buffer[2] = 0xef;
      buffer[3] = 0xfa;
      strictEqual(buffer.readInt32BE(0), -69638);
      strictEqual(buffer.readInt32LE(0), -84934913);

      buffer[0] = 0x42;
      buffer[1] = 0xc3;
      buffer[2] = 0x95;
      buffer[3] = 0xa9;
      strictEqual(buffer.readInt32BE(0), 0x42c395a9);
      strictEqual(buffer.readInt32BE(1), -1013601994);
      strictEqual(buffer.readInt32BE(2), -1784072681);
      strictEqual(buffer.readInt32LE(0), -1449802942);
      strictEqual(buffer.readInt32LE(1), 917083587);
      strictEqual(buffer.readInt32LE(2), 389458325);
    }

    // Test Int
    {
      const buffer = Buffer.from([
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      ]);

      strictEqual(buffer.readIntLE(0, 1), 0x01);
      strictEqual(buffer.readIntBE(0, 1), 0x01);
      strictEqual(buffer.readIntLE(0, 3), 0x030201);
      strictEqual(buffer.readIntBE(0, 3), 0x010203);
      strictEqual(buffer.readIntLE(0, 5), 0x0504030201);
      strictEqual(buffer.readIntBE(0, 5), 0x0102030405);
      strictEqual(buffer.readIntLE(0, 6), 0x060504030201);
      strictEqual(buffer.readIntBE(0, 6), 0x010203040506);
      strictEqual(buffer.readIntLE(1, 6), 0x070605040302);
      strictEqual(buffer.readIntBE(1, 6), 0x020304050607);
      strictEqual(buffer.readIntLE(2, 6), 0x080706050403);
      strictEqual(buffer.readIntBE(2, 6), 0x030405060708);

      // Check byteLength.
      ['readIntBE', 'readIntLE'].forEach((fn) => {
        ['', '0', null, {}, [], () => {}, true, false, undefined].forEach(
          (len) => {
            throws(() => buffer[fn](0, len), { name: 'RangeError' });
          }
        );

        [Infinity, -1].forEach((byteLength) => {
          throws(() => buffer[fn](0, byteLength), {
            name: 'RangeError',
          });
        });

        [NaN, 1.01].forEach((byteLength) => {
          throws(() => buffer[fn](0, byteLength), {
            name: 'RangeError',
          });
        });
      });

      // Test 1 to 6 bytes.
      for (let i = 1; i <= 6; i++) {
        ['readIntBE', 'readIntLE'].forEach((fn) => {
          ['', '0', null, {}, [], () => {}, true, false, undefined].forEach(
            (o) => {
              throws(() => buffer[fn](o, i), {
                name: 'TypeError',
              });
            }
          );

          [Infinity, -1, -4294967295].forEach((offset) => {
            throws(() => buffer[fn](offset, i), {
              name: 'RangeError',
            });
          });

          [NaN, 1.01].forEach((offset) => {
            throws(() => buffer[fn](offset, i), {
              name: 'RangeError',
            });
          });
        });
      }
    }
  },
};

export const readUint = {
  test(ctrl, env, ctx) {
    // Test OOB
    {
      const buffer = Buffer.alloc(4);

      ['UInt8', 'UInt16BE', 'UInt16LE', 'UInt32BE', 'UInt32LE'].forEach(
        (fn) => {
          // Verify that default offset works fine.
          buffer[`read${fn}`](undefined);
          buffer[`read${fn}`]();

          ['', '0', null, {}, [], () => {}, true, false].forEach((o) => {
            throws(() => buffer[`read${fn}`](o), {
              code: 'ERR_INVALID_ARG_TYPE',
              name: 'TypeError',
            });
          });

          [Infinity, -1, -4294967295].forEach((offset) => {
            throws(() => buffer[`read${fn}`](offset), {
              code: 'ERR_OUT_OF_RANGE',
              name: 'RangeError',
            });
          });

          [NaN, 1.01].forEach((offset) => {
            throws(() => buffer[`read${fn}`](offset), {
              code: 'ERR_OUT_OF_RANGE',
              name: 'RangeError',
            });
          });
        }
      );
    }

    // Test 8 bit unsigned integers
    {
      const data = Buffer.from([0xff, 0x2a, 0x2a, 0x2a]);
      strictEqual(data.readUInt8(0), 255);
      strictEqual(data.readUInt8(1), 42);
      strictEqual(data.readUInt8(2), 42);
      strictEqual(data.readUInt8(3), 42);
    }

    // Test 16 bit unsigned integers
    {
      const data = Buffer.from([0x00, 0x2a, 0x42, 0x3f]);
      strictEqual(data.readUInt16BE(0), 0x2a);
      strictEqual(data.readUInt16BE(1), 0x2a42);
      strictEqual(data.readUInt16BE(2), 0x423f);
      strictEqual(data.readUInt16LE(0), 0x2a00);
      strictEqual(data.readUInt16LE(1), 0x422a);
      strictEqual(data.readUInt16LE(2), 0x3f42);

      data[0] = 0xfe;
      data[1] = 0xfe;
      strictEqual(data.readUInt16BE(0), 0xfefe);
      strictEqual(data.readUInt16LE(0), 0xfefe);
    }

    // Test 32 bit unsigned integers
    {
      const data = Buffer.from([0x32, 0x65, 0x42, 0x56, 0x23, 0xff]);
      strictEqual(data.readUInt32BE(0), 0x32654256);
      strictEqual(data.readUInt32BE(1), 0x65425623);
      strictEqual(data.readUInt32BE(2), 0x425623ff);
      strictEqual(data.readUInt32LE(0), 0x56426532);
      strictEqual(data.readUInt32LE(1), 0x23564265);
      strictEqual(data.readUInt32LE(2), 0xff235642);
    }

    // Test UInt
    {
      const buffer = Buffer.from([
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      ]);

      strictEqual(buffer.readUIntLE(0, 1), 0x01);
      strictEqual(buffer.readUIntBE(0, 1), 0x01);
      strictEqual(buffer.readUIntLE(0, 3), 0x030201);
      strictEqual(buffer.readUIntBE(0, 3), 0x010203);
      strictEqual(buffer.readUIntLE(0, 5), 0x0504030201);
      strictEqual(buffer.readUIntBE(0, 5), 0x0102030405);
      strictEqual(buffer.readUIntLE(0, 6), 0x060504030201);
      strictEqual(buffer.readUIntBE(0, 6), 0x010203040506);
      strictEqual(buffer.readUIntLE(1, 6), 0x070605040302);
      strictEqual(buffer.readUIntBE(1, 6), 0x020304050607);
      strictEqual(buffer.readUIntLE(2, 6), 0x080706050403);
      strictEqual(buffer.readUIntBE(2, 6), 0x030405060708);

      // Check byteLength.
      ['readUIntBE', 'readUIntLE'].forEach((fn) => {
        ['', '0', null, {}, [], () => {}, true, false, undefined].forEach(
          (len) => {
            throws(() => buffer[fn](0, len), { name: 'RangeError' });
          }
        );

        [Infinity, -1].forEach((byteLength) => {
          throws(() => buffer[fn](0, byteLength), {
            name: 'RangeError',
          });
        });

        [NaN, 1.01].forEach((byteLength) => {
          throws(() => buffer[fn](0, byteLength), {
            name: 'RangeError',
          });
        });
      });

      // Test 1 to 6 bytes.
      for (let i = 1; i <= 6; i++) {
        ['readUIntBE', 'readUIntLE'].forEach((fn) => {
          ['', '0', null, {}, [], () => {}, true, false, undefined].forEach(
            (o) => {
              throws(() => buffer[fn](o, i), {
                name: 'TypeError',
              });
            }
          );

          [Infinity, -1, -4294967295].forEach((offset) => {
            throws(() => buffer[fn](offset, i), {
              name: 'RangeError',
            });
          });

          [NaN, 1.01].forEach((offset) => {
            throws(() => buffer[fn](offset, i), {
              name: 'RangeError',
            });
          });
        });
      }
    }
  },
};

export const sharedArrayBuffer = {
  test(ctrl, env, ctx) {
    const sab = new SharedArrayBuffer(24);
    const arr1 = new Uint16Array(sab);
    const arr2 = new Uint16Array(12);
    arr2[0] = 5000;
    arr1[0] = 5000;
    arr1[1] = 4000;
    arr2[1] = 4000;

    const arr_buf = Buffer.from(arr1.buffer);
    const ar_buf = Buffer.from(arr2.buffer);

    deepStrictEqual(arr_buf, ar_buf);

    arr1[1] = 6000;
    arr2[1] = 6000;

    deepStrictEqual(arr_buf, ar_buf);

    // Checks for calling Buffer.byteLength on a SharedArrayBuffer.
    strictEqual(Buffer.byteLength(sab), sab.byteLength);

    Buffer.from({ buffer: sab }); // Should not throw.
  },
};

export const slice = {
  test(ctrl, env, ctx) {
    strictEqual(Buffer.from('hello', 'utf8').slice(0, 0).length, 0);
    strictEqual(Buffer('hello', 'utf8').slice(0, 0).length, 0);

    const buf = Buffer.from('0123456789', 'utf8');
    const expectedSameBufs = [
      [buf.slice(-10, 10), Buffer.from('0123456789', 'utf8')],
      [buf.slice(-20, 10), Buffer.from('0123456789', 'utf8')],
      [buf.slice(-20, -10), Buffer.from('', 'utf8')],
      [buf.slice(), Buffer.from('0123456789', 'utf8')],
      [buf.slice(0), Buffer.from('0123456789', 'utf8')],
      [buf.slice(0, 0), Buffer.from('', 'utf8')],
      [buf.slice(undefined), Buffer.from('0123456789', 'utf8')],
      [buf.slice('foobar'), Buffer.from('0123456789', 'utf8')],
      [buf.slice(undefined, undefined), Buffer.from('0123456789', 'utf8')],
      [buf.slice(2), Buffer.from('23456789', 'utf8')],
      [buf.slice(5), Buffer.from('56789', 'utf8')],
      [buf.slice(10), Buffer.from('', 'utf8')],
      [buf.slice(5, 8), Buffer.from('567', 'utf8')],
      [buf.slice(8, -1), Buffer.from('8', 'utf8')],
      [buf.slice(-10), Buffer.from('0123456789', 'utf8')],
      [buf.slice(0, -9), Buffer.from('0', 'utf8')],
      [buf.slice(0, -10), Buffer.from('', 'utf8')],
      [buf.slice(0, -1), Buffer.from('012345678', 'utf8')],
      [buf.slice(2, -2), Buffer.from('234567', 'utf8')],
      [buf.slice(0, 65536), Buffer.from('0123456789', 'utf8')],
      [buf.slice(65536, 0), Buffer.from('', 'utf8')],
      [buf.slice(-5, -8), Buffer.from('', 'utf8')],
      [buf.slice(-5, -3), Buffer.from('56', 'utf8')],
      [buf.slice(-10, 10), Buffer.from('0123456789', 'utf8')],
      [buf.slice('0', '1'), Buffer.from('0', 'utf8')],
      [buf.slice('-5', '10'), Buffer.from('56789', 'utf8')],
      [buf.slice('-10', '10'), Buffer.from('0123456789', 'utf8')],
      [buf.slice('-10', '-5'), Buffer.from('01234', 'utf8')],
      [buf.slice('-10', '-0'), Buffer.from('', 'utf8')],
      [buf.slice('111'), Buffer.from('', 'utf8')],
      [buf.slice('0', '-111'), Buffer.from('', 'utf8')],
    ];

    for (let i = 0, s = buf.toString(); i < buf.length; ++i) {
      expectedSameBufs.push(
        [buf.slice(i), Buffer.from(s.slice(i))],
        [buf.slice(0, i), Buffer.from(s.slice(0, i))],
        [buf.slice(-i), Buffer.from(s.slice(-i))],
        [buf.slice(0, -i), Buffer.from(s.slice(0, -i))]
      );
    }

    expectedSameBufs.forEach(([buf1, buf2]) => {
      strictEqual(Buffer.compare(buf1, buf2), 0);
    });

    const utf16Buf = Buffer.from('0123456789', 'utf16le');
    deepStrictEqual(utf16Buf.slice(0, 6), Buffer.from('012', 'utf16le'));
    // Try to slice a zero length Buffer.
    // See https://github.com/joyent/node/issues/5881
    strictEqual(Buffer.alloc(0).slice(0, 1).length, 0);

    {
      // Single argument slice
      strictEqual(
        Buffer.from('abcde', 'utf8').slice(1).toString('utf8'),
        'bcde'
      );
    }

    // slice(0,0).length === 0
    strictEqual(Buffer.from('hello', 'utf8').slice(0, 0).length, 0);

    {
      // Regression tests for https://github.com/nodejs/node/issues/9096
      const buf = Buffer.from('abcd', 'utf8');
      strictEqual(buf.slice(buf.length / 3).toString('utf8'), 'bcd');
      strictEqual(buf.slice(buf.length / 3, buf.length).toString(), 'bcd');
    }

    {
      const buf = Buffer.from('abcdefg', 'utf8');
      strictEqual(
        buf.slice(-(-1 >>> 0) - 1).toString('utf8'),
        buf.toString('utf8')
      );
    }

    {
      const buf = Buffer.from('abc', 'utf8');
      strictEqual(buf.slice(-0.5).toString('utf8'), buf.toString('utf8'));
    }

    {
      const buf = Buffer.from([
        1, 29, 0, 0, 1, 143, 216, 162, 92, 254, 248, 63, 0, 0, 0, 18, 184, 6, 0,
        175, 29, 0, 8, 11, 1, 0, 0,
      ]);
      const chunk1 = Buffer.from([
        1, 29, 0, 0, 1, 143, 216, 162, 92, 254, 248, 63, 0,
      ]);
      const chunk2 = Buffer.from([
        0, 0, 18, 184, 6, 0, 175, 29, 0, 8, 11, 1, 0, 0,
      ]);
      const middle = buf.length / 2;

      deepStrictEqual(buf.slice(0, middle), chunk1);
      deepStrictEqual(buf.slice(middle), chunk2);
    }
  },
};

export const swap = {
  test(ctrl, env, ctx) {
    // Test buffers small enough to use the JS implementation
    {
      const buf = Buffer.from([
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
        0x0d, 0x0e, 0x0f, 0x10,
      ]);

      strictEqual(buf, buf.swap16());
      deepStrictEqual(
        buf,
        Buffer.from([
          0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0x08, 0x07, 0x0a, 0x09, 0x0c,
          0x0b, 0x0e, 0x0d, 0x10, 0x0f,
        ])
      );
      buf.swap16(); // restore

      strictEqual(buf, buf.swap32());
      deepStrictEqual(
        buf,
        Buffer.from([
          0x04, 0x03, 0x02, 0x01, 0x08, 0x07, 0x06, 0x05, 0x0c, 0x0b, 0x0a,
          0x09, 0x10, 0x0f, 0x0e, 0x0d,
        ])
      );
      buf.swap32(); // restore

      strictEqual(buf, buf.swap64());
      deepStrictEqual(
        buf,
        Buffer.from([
          0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x10, 0x0f, 0x0e,
          0x0d, 0x0c, 0x0b, 0x0a, 0x09,
        ])
      );
    }

    // Operates in-place
    {
      const buf = Buffer.from([0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7]);
      buf.slice(1, 5).swap32();
      deepStrictEqual(buf, Buffer.from([0x1, 0x5, 0x4, 0x3, 0x2, 0x6, 0x7]));
      buf.slice(1, 5).swap16();
      deepStrictEqual(buf, Buffer.from([0x1, 0x4, 0x5, 0x2, 0x3, 0x6, 0x7]));

      // Length assertions
      const re16 = /Buffer size must be a multiple of 16-bits/;
      const re32 = /Buffer size must be a multiple of 32-bits/;
      const re64 = /Buffer size must be a multiple of 64-bits/;

      throws(() => Buffer.from(buf).swap16(), re16);
      throws(() => Buffer.alloc(1025).swap16(), re16);
      throws(() => Buffer.from(buf).swap32(), re32);
      throws(() => buf.slice(1, 3).swap32(), re32);
      throws(() => Buffer.alloc(1025).swap32(), re32);
      throws(() => buf.slice(1, 3).swap64(), re64);
      throws(() => Buffer.alloc(1025).swap64(), re64);
    }

    {
      const buf = Buffer.from([
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
        0x0d, 0x0e, 0x0f, 0x10, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
      ]);

      buf.slice(2, 18).swap64();

      deepStrictEqual(
        buf,
        Buffer.from([
          0x01, 0x02, 0x0a, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
          0x01, 0x10, 0x0f, 0x0e, 0x0d, 0x0c, 0x0b, 0x03, 0x04, 0x05, 0x06,
          0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        ])
      );
    }

    // Force use of native code (Buffer size above threshold limit for js impl)
    {
      const bufData = new Uint32Array(256).fill(0x04030201);
      const buf = Buffer.from(bufData.buffer, bufData.byteOffset);
      const otherBufData = new Uint32Array(256).fill(0x03040102);
      const otherBuf = Buffer.from(
        otherBufData.buffer,
        otherBufData.byteOffset
      );
      buf.swap16();
      deepStrictEqual(buf, otherBuf);
    }

    {
      const bufData = new Uint32Array(256).fill(0x04030201);
      const buf = Buffer.from(bufData.buffer);
      const otherBufData = new Uint32Array(256).fill(0x01020304);
      const otherBuf = Buffer.from(
        otherBufData.buffer,
        otherBufData.byteOffset
      );
      buf.swap32();
      deepStrictEqual(buf, otherBuf);
    }

    {
      const bufData = new Uint8Array(256 * 8);
      const otherBufData = new Uint8Array(256 * 8);
      for (let i = 0; i < bufData.length; i++) {
        bufData[i] = i % 8;
        otherBufData[otherBufData.length - i - 1] = i % 8;
      }
      const buf = Buffer.from(bufData.buffer, bufData.byteOffset);
      const otherBuf = Buffer.from(
        otherBufData.buffer,
        otherBufData.byteOffset
      );
      buf.swap64();
      deepStrictEqual(buf, otherBuf);
    }

    // Test native code with buffers that are not memory-aligned
    {
      const bufData = new Uint8Array(256 * 8);
      const otherBufData = new Uint8Array(256 * 8 - 2);
      for (let i = 0; i < bufData.length; i++) {
        bufData[i] = i % 2;
      }
      for (let i = 1; i < otherBufData.length; i++) {
        otherBufData[otherBufData.length - i] = (i + 1) % 2;
      }
      const buf = Buffer.from(bufData.buffer, bufData.byteOffset);
      // 0|1 0|1 0|1...
      const otherBuf = Buffer.from(
        otherBufData.buffer,
        otherBufData.byteOffset
      );
      // 0|0 1|0 1|0...

      buf.slice(1, buf.length - 1).swap16();
      deepStrictEqual(buf.slice(0, otherBuf.length), otherBuf);
    }

    {
      const bufData = new Uint8Array(256 * 8);
      const otherBufData = new Uint8Array(256 * 8 - 4);
      for (let i = 0; i < bufData.length; i++) {
        bufData[i] = i % 4;
      }
      for (let i = 1; i < otherBufData.length; i++) {
        otherBufData[otherBufData.length - i] = (i + 1) % 4;
      }
      const buf = Buffer.from(bufData.buffer, bufData.byteOffset);
      // 0|1 2 3 0|1 2 3...
      const otherBuf = Buffer.from(
        otherBufData.buffer,
        otherBufData.byteOffset
      );
      // 0|0 3 2 1|0 3 2...

      buf.slice(1, buf.length - 3).swap32();
      deepStrictEqual(buf.slice(0, otherBuf.length), otherBuf);
    }

    {
      const bufData = new Uint8Array(256 * 8);
      const otherBufData = new Uint8Array(256 * 8 - 8);
      for (let i = 0; i < bufData.length; i++) {
        bufData[i] = i % 8;
      }
      for (let i = 1; i < otherBufData.length; i++) {
        otherBufData[otherBufData.length - i] = (i + 1) % 8;
      }
      const buf = Buffer.from(bufData.buffer, bufData.byteOffset);
      // 0|1 2 3 4 5 6 7 0|1 2 3 4...
      const otherBuf = Buffer.from(
        otherBufData.buffer,
        otherBufData.byteOffset
      );
      // 0|0 7 6 5 4 3 2 1|0 7 6 5...

      buf.slice(1, buf.length - 7).swap64();
      deepStrictEqual(buf.slice(0, otherBuf.length), otherBuf);
    }
  },
};

export const json = {
  test(ctrl, env, ctx) {
    {
      strictEqual(
        JSON.stringify(Buffer.alloc(0)),
        '{"type":"Buffer","data":[]}'
      );
      strictEqual(
        JSON.stringify(Buffer.from([1, 2, 3, 4])),
        '{"type":"Buffer","data":[1,2,3,4]}'
      );
    }

    // issue GH-7849
    {
      const buf = Buffer.from('test');
      const json = JSON.stringify(buf);
      const obj = JSON.parse(json);
      const copy = Buffer.from(obj);

      deepStrictEqual(buf, copy);
    }

    // GH-5110
    {
      const buffer = Buffer.from('test');
      const string = JSON.stringify(buffer);

      strictEqual(string, '{"type":"Buffer","data":[116,101,115,116]}');

      function receiver(key, value) {
        return value && value.type === 'Buffer'
          ? Buffer.from(value.data)
          : value;
      }

      deepStrictEqual(buffer, JSON.parse(string, receiver));
    }
  },
};

export const writeUint8 = {
  test(ctrl, env, ctx) {
    {
      // OOB
      const data = Buffer.alloc(8);
      ['UInt8', 'UInt16BE', 'UInt16LE', 'UInt32BE', 'UInt32LE'].forEach(
        (fn) => {
          // Verify that default offset works fine.
          data[`write${fn}`](23, undefined);
          data[`write${fn}`](23);

          ['', '0', null, {}, [], () => {}, true, false].forEach((o) => {
            throws(() => data[`write${fn}`](23, o), { name: 'TypeError' });
          });

          [NaN, Infinity, -1, 1.01].forEach((o) => {
            throws(() => data[`write${fn}`](23, o), { name: 'RangeError' });
          });
        }
      );
    }

    {
      // Test 8 bit
      const data = Buffer.alloc(4);

      data.writeUInt8(23, 0);
      data.writeUInt8(23, 1);
      data.writeUInt8(23, 2);
      data.writeUInt8(23, 3);
      ok(data.equals(new Uint8Array([23, 23, 23, 23])));

      data.writeUInt8(23, 0);
      data.writeUInt8(23, 1);
      data.writeUInt8(23, 2);
      data.writeUInt8(23, 3);
      ok(data.equals(new Uint8Array([23, 23, 23, 23])));

      data.writeUInt8(255, 0);
      strictEqual(data[0], 255);

      data.writeUInt8(255, 0);
      strictEqual(data[0], 255);
    }

    // Test 16 bit
    {
      let value = 0x2343;
      const data = Buffer.alloc(4);

      data.writeUInt16BE(value, 0);
      ok(data.equals(new Uint8Array([0x23, 0x43, 0, 0])));

      data.writeUInt16BE(value, 1);
      ok(data.equals(new Uint8Array([0x23, 0x23, 0x43, 0])));

      data.writeUInt16BE(value, 2);
      ok(data.equals(new Uint8Array([0x23, 0x23, 0x23, 0x43])));

      data.writeUInt16LE(value, 0);
      ok(data.equals(new Uint8Array([0x43, 0x23, 0x23, 0x43])));

      data.writeUInt16LE(value, 1);
      ok(data.equals(new Uint8Array([0x43, 0x43, 0x23, 0x43])));

      data.writeUInt16LE(value, 2);
      ok(data.equals(new Uint8Array([0x43, 0x43, 0x43, 0x23])));

      value = 0xff80;
      data.writeUInt16LE(value, 0);
      ok(data.equals(new Uint8Array([0x80, 0xff, 0x43, 0x23])));

      data.writeUInt16BE(value, 0);
      ok(data.equals(new Uint8Array([0xff, 0x80, 0x43, 0x23])));

      value = 0xfffff;
      ['writeUInt16BE', 'writeUInt16LE'].forEach((fn) => {
        throws(() => data[fn](value, 0), {
          name: 'RangeError',
        });
      });
    }

    // Test 32 bit
    {
      const data = Buffer.alloc(6);
      const value = 0xe7f90a6d;

      data.writeUInt32BE(value, 0);
      ok(data.equals(new Uint8Array([0xe7, 0xf9, 0x0a, 0x6d, 0, 0])));

      data.writeUInt32BE(value, 1);
      ok(data.equals(new Uint8Array([0xe7, 0xe7, 0xf9, 0x0a, 0x6d, 0])));

      data.writeUInt32BE(value, 2);
      ok(data.equals(new Uint8Array([0xe7, 0xe7, 0xe7, 0xf9, 0x0a, 0x6d])));

      data.writeUInt32LE(value, 0);
      ok(data.equals(new Uint8Array([0x6d, 0x0a, 0xf9, 0xe7, 0x0a, 0x6d])));

      data.writeUInt32LE(value, 1);
      ok(data.equals(new Uint8Array([0x6d, 0x6d, 0x0a, 0xf9, 0xe7, 0x6d])));

      data.writeUInt32LE(value, 2);
      ok(data.equals(new Uint8Array([0x6d, 0x6d, 0x6d, 0x0a, 0xf9, 0xe7])));
    }

    // Test 48 bit
    {
      const value = 0x1234567890ab;
      const data = Buffer.allocUnsafe(6);
      data.writeUIntBE(value, 0, 6);
      ok(data.equals(new Uint8Array([0x12, 0x34, 0x56, 0x78, 0x90, 0xab])));

      data.writeUIntLE(value, 0, 6);
      ok(data.equals(new Uint8Array([0xab, 0x90, 0x78, 0x56, 0x34, 0x12])));
    }

    // Test UInt
    {
      const data = Buffer.alloc(8);
      let val = 0x100;

      // Check byteLength.
      ['writeUIntBE', 'writeUIntLE'].forEach((fn) => {
        ['', '0', null, {}, [], () => {}, true, false, undefined].forEach(
          (bl) => {
            throws(() => data[fn](23, 0, bl), { name: 'RangeError' });
          }
        );

        [Infinity, -1].forEach((byteLength) => {
          throws(() => data[fn](23, 0, byteLength), {
            name: 'RangeError',
          });
        });

        [NaN, 1.01].forEach((byteLength) => {
          throws(() => data[fn](42, 0, byteLength), {
            name: 'RangeError',
          });
        });
      });

      // Test 1 to 6 bytes.
      for (let i = 1; i <= 6; i++) {
        const range = i < 5 ? `= ${val - 1}` : ` 2 ** ${i * 8}`;
        const received =
          i > 4 ? String(val).replace(/(\d)(?=(\d\d\d)+(?!\d))/g, '$1_') : val;
        ['writeUIntBE', 'writeUIntLE'].forEach((fn) => {
          throws(
            () => {
              data[fn](val, 0, i);
            },
            {
              name: 'RangeError',
            }
          );

          ['', '0', null, {}, [], () => {}, true, false].forEach((o) => {
            throws(() => data[fn](23, o, i), {
              name: 'TypeError',
            });
          });

          [Infinity, -1, -4294967295].forEach((offset) => {
            throws(() => data[fn](val - 1, offset, i), {
              name: 'RangeError',
            });
          });

          [NaN, 1.01].forEach((offset) => {
            throws(() => data[fn](val - 1, offset, i), {
              name: 'RangeError',
            });
          });
        });

        val *= 0x100;
      }
    }

    for (const fn of [
      'UInt8',
      'UInt16LE',
      'UInt16BE',
      'UInt32LE',
      'UInt32BE',
      'UIntLE',
      'UIntBE',
      'BigUInt64LE',
      'BigUInt64BE',
    ]) {
      const p = Buffer.prototype;
      const lowerFn = fn.replace(/UInt/, 'Uint');
      strictEqual(p[`write${fn}`], p[`write${lowerFn}`]);
      strictEqual(p[`read${fn}`], p[`read${lowerFn}`]);
    }
  },
};

export const writeInt = {
  test(ctrl, env, ctx) {
    const errorOutOfBounds = {
      name: 'RangeError',
    };

    // Test 8 bit
    {
      const buffer = Buffer.alloc(2);

      buffer.writeInt8(0x23, 0);
      buffer.writeInt8(-5, 1);
      ok(buffer.equals(new Uint8Array([0x23, 0xfb])));

      /* Make sure we handle min/max correctly */
      buffer.writeInt8(0x7f, 0);
      buffer.writeInt8(-0x80, 1);
      ok(buffer.equals(new Uint8Array([0x7f, 0x80])));

      throws(() => {
        buffer.writeInt8(0x7f + 1, 0);
      }, errorOutOfBounds);
      throws(() => {
        buffer.writeInt8(-0x80 - 1, 0);
      }, errorOutOfBounds);

      // Verify that default offset works fine.
      buffer.writeInt8(23, undefined);
      buffer.writeInt8(23);

      ['', '0', null, {}, [], () => {}, true, false].forEach((off) => {
        throws(() => buffer.writeInt8(23, off), { name: 'TypeError' });
      });

      [NaN, Infinity, -1, 1.01].forEach((off) => {
        throws(() => buffer.writeInt8(23, off), { name: 'RangeError' });
      });
    }

    // Test 16 bit
    {
      const buffer = Buffer.alloc(4);

      buffer.writeInt16BE(0x0023, 0);
      buffer.writeInt16LE(0x0023, 2);
      ok(buffer.equals(new Uint8Array([0x00, 0x23, 0x23, 0x00])));

      buffer.writeInt16BE(-5, 0);
      buffer.writeInt16LE(-5, 2);
      ok(buffer.equals(new Uint8Array([0xff, 0xfb, 0xfb, 0xff])));

      buffer.writeInt16BE(-1679, 0);
      buffer.writeInt16LE(-1679, 2);
      ok(buffer.equals(new Uint8Array([0xf9, 0x71, 0x71, 0xf9])));

      /* Make sure we handle min/max correctly */
      buffer.writeInt16BE(0x7fff, 0);
      buffer.writeInt16BE(-0x8000, 2);
      ok(buffer.equals(new Uint8Array([0x7f, 0xff, 0x80, 0x00])));

      buffer.writeInt16LE(0x7fff, 0);
      buffer.writeInt16LE(-0x8000, 2);
      ok(buffer.equals(new Uint8Array([0xff, 0x7f, 0x00, 0x80])));

      ['writeInt16BE', 'writeInt16LE'].forEach((fn) => {
        // Verify that default offset works fine.
        buffer[fn](23, undefined);
        buffer[fn](23);

        throws(() => {
          buffer[fn](0x7fff + 1, 0);
        }, errorOutOfBounds);
        throws(() => {
          buffer[fn](-0x8000 - 1, 0);
        }, errorOutOfBounds);

        ['', '0', null, {}, [], () => {}, true, false].forEach((off) => {
          throws(() => buffer[fn](23, off), { code: 'ERR_INVALID_ARG_TYPE' });
        });

        [NaN, Infinity, -1, 1.01].forEach((off) => {
          throws(() => buffer[fn](23, off), { code: 'ERR_OUT_OF_RANGE' });
        });
      });
    }

    // Test 32 bit
    {
      const buffer = Buffer.alloc(8);

      buffer.writeInt32BE(0x23, 0);
      buffer.writeInt32LE(0x23, 4);
      ok(
        buffer.equals(
          new Uint8Array([0x00, 0x00, 0x00, 0x23, 0x23, 0x00, 0x00, 0x00])
        )
      );

      buffer.writeInt32BE(-5, 0);
      buffer.writeInt32LE(-5, 4);
      ok(
        buffer.equals(
          new Uint8Array([0xff, 0xff, 0xff, 0xfb, 0xfb, 0xff, 0xff, 0xff])
        )
      );

      buffer.writeInt32BE(-805306713, 0);
      buffer.writeInt32LE(-805306713, 4);
      ok(
        buffer.equals(
          new Uint8Array([0xcf, 0xff, 0xfe, 0xa7, 0xa7, 0xfe, 0xff, 0xcf])
        )
      );

      /* Make sure we handle min/max correctly */
      buffer.writeInt32BE(0x7fffffff, 0);
      buffer.writeInt32BE(-0x80000000, 4);
      ok(
        buffer.equals(
          new Uint8Array([0x7f, 0xff, 0xff, 0xff, 0x80, 0x00, 0x00, 0x00])
        )
      );

      buffer.writeInt32LE(0x7fffffff, 0);
      buffer.writeInt32LE(-0x80000000, 4);
      ok(
        buffer.equals(
          new Uint8Array([0xff, 0xff, 0xff, 0x7f, 0x00, 0x00, 0x00, 0x80])
        )
      );

      ['writeInt32BE', 'writeInt32LE'].forEach((fn) => {
        // Verify that default offset works fine.
        buffer[fn](23, undefined);
        buffer[fn](23);

        throws(() => {
          buffer[fn](0x7fffffff + 1, 0);
        }, errorOutOfBounds);
        throws(() => {
          buffer[fn](-0x80000000 - 1, 0);
        }, errorOutOfBounds);

        ['', '0', null, {}, [], () => {}, true, false].forEach((off) => {
          throws(() => buffer[fn](23, off), { code: 'ERR_INVALID_ARG_TYPE' });
        });

        [NaN, Infinity, -1, 1.01].forEach((off) => {
          throws(() => buffer[fn](23, off), { code: 'ERR_OUT_OF_RANGE' });
        });
      });
    }

    // Test 48 bit
    {
      const value = 0x1234567890ab;
      const buffer = Buffer.allocUnsafe(6);
      buffer.writeIntBE(value, 0, 6);
      ok(buffer.equals(new Uint8Array([0x12, 0x34, 0x56, 0x78, 0x90, 0xab])));

      buffer.writeIntLE(value, 0, 6);
      ok(buffer.equals(new Uint8Array([0xab, 0x90, 0x78, 0x56, 0x34, 0x12])));
    }

    // Test Int
    {
      const data = Buffer.alloc(8);

      // Check byteLength.
      ['writeIntBE', 'writeIntLE'].forEach((fn) => {
        ['', '0', null, {}, [], () => {}, true, false, undefined].forEach(
          (bl) => {
            throws(() => data[fn](23, 0, bl), { name: 'RangeError' });
          }
        );

        [Infinity, -1].forEach((byteLength) => {
          throws(() => data[fn](23, 0, byteLength), {
            name: 'RangeError',
          });
        });

        [NaN, 1.01].forEach((byteLength) => {
          throws(() => data[fn](42, 0, byteLength), {
            name: 'RangeError',
          });
        });
      });

      // Test 1 to 6 bytes.
      for (let i = 1; i <= 6; i++) {
        ['writeIntBE', 'writeIntLE'].forEach((fn) => {
          const min = -(2 ** (i * 8 - 1));
          const max = 2 ** (i * 8 - 1) - 1;
          let range = `>= ${min} and <= ${max}`;
          if (i > 4) {
            range = `>= -(2 ** ${i * 8 - 1}) and < 2 ** ${i * 8 - 1}`;
          }
          [min - 1, max + 1].forEach((val) => {
            const received =
              i > 4
                ? String(val).replace(/(\d)(?=(\d\d\d)+(?!\d))/g, '$1_')
                : val;
            throws(
              () => {
                data[fn](val, 0, i);
              },
              {
                name: 'RangeError',
              }
            );
          });

          ['', '0', null, {}, [], () => {}, true, false, undefined].forEach(
            (o) => {
              throws(() => data[fn](min, o, i), {
                name: 'TypeError',
              });
            }
          );

          [Infinity, -1, -4294967295].forEach((offset) => {
            throws(() => data[fn](min, offset, i), {
              name: 'RangeError',
            });
          });

          [NaN, 1.01].forEach((offset) => {
            throws(() => data[fn](max, offset, i), {
              name: 'RangeError',
            });
          });
        });
      }
    }
  },
};

export const writeFloat = {
  test(ctrl, env, ctx) {
    const buffer = Buffer.allocUnsafe(8);

    buffer.writeFloatBE(1, 0);
    buffer.writeFloatLE(1, 4);
    ok(
      buffer.equals(
        new Uint8Array([0x3f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f])
      )
    );

    buffer.writeFloatBE(1 / 3, 0);
    buffer.writeFloatLE(1 / 3, 4);
    ok(
      buffer.equals(
        new Uint8Array([0x3e, 0xaa, 0xaa, 0xab, 0xab, 0xaa, 0xaa, 0x3e])
      )
    );

    buffer.writeFloatBE(3.4028234663852886e38, 0);
    buffer.writeFloatLE(3.4028234663852886e38, 4);
    ok(
      buffer.equals(
        new Uint8Array([0x7f, 0x7f, 0xff, 0xff, 0xff, 0xff, 0x7f, 0x7f])
      )
    );

    buffer.writeFloatLE(1.1754943508222875e-38, 0);
    buffer.writeFloatBE(1.1754943508222875e-38, 4);
    ok(
      buffer.equals(
        new Uint8Array([0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0x00, 0x00])
      )
    );

    buffer.writeFloatBE(0 * -1, 0);
    buffer.writeFloatLE(0 * -1, 4);
    ok(
      buffer.equals(
        new Uint8Array([0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80])
      )
    );

    buffer.writeFloatBE(Infinity, 0);
    buffer.writeFloatLE(Infinity, 4);
    ok(
      buffer.equals(
        new Uint8Array([0x7f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x7f])
      )
    );

    strictEqual(buffer.readFloatBE(0), Infinity);
    strictEqual(buffer.readFloatLE(4), Infinity);

    buffer.writeFloatBE(-Infinity, 0);
    buffer.writeFloatLE(-Infinity, 4);
    ok(
      buffer.equals(
        new Uint8Array([0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0xff])
      )
    );

    strictEqual(buffer.readFloatBE(0), -Infinity);
    strictEqual(buffer.readFloatLE(4), -Infinity);

    buffer.writeFloatBE(NaN, 0);
    buffer.writeFloatLE(NaN, 4);

    // JS only knows a single NaN but there exist two platform specific
    // implementations. Therefore, allow both quiet and signalling NaNs.
    if (buffer[1] === 0xbf) {
      ok(
        buffer.equals(
          new Uint8Array([0x7f, 0xbf, 0xff, 0xff, 0xff, 0xff, 0xbf, 0x7f])
        )
      );
    } else {
      ok(
        buffer.equals(
          new Uint8Array([0x7f, 0xc0, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x7f])
        )
      );
    }

    ok(Number.isNaN(buffer.readFloatBE(0)));
    ok(Number.isNaN(buffer.readFloatLE(4)));

    // OOB in writeFloat{LE,BE} should throw.
    {
      const small = Buffer.allocUnsafe(1);

      ['writeFloatLE', 'writeFloatBE'].forEach((fn) => {
        // Verify that default offset works fine.
        buffer[fn](23, undefined);
        buffer[fn](23);

        throws(() => small[fn](11.11, 0), {
          name: 'RangeError',
        });

        ['', '0', null, {}, [], () => {}, true, false].forEach((off) => {
          throws(() => small[fn](23, off), { name: 'TypeError' });
        });

        [Infinity, -1, 5].forEach((offset) => {
          throws(() => buffer[fn](23, offset), {
            name: 'RangeError',
          });
        });

        [NaN, 1.01].forEach((offset) => {
          throws(() => buffer[fn](42, offset), {
            name: 'RangeError',
          });
        });
      });
    }
  },
};

export const writeDouble = {
  test(ctrl, env, ctx) {
    const buffer = Buffer.allocUnsafe(16);

    buffer.writeDoubleBE(2.225073858507201e-308, 0);
    buffer.writeDoubleLE(2.225073858507201e-308, 8);
    ok(
      buffer.equals(
        new Uint8Array([
          0x00, 0x0f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff, 0xff, 0x0f, 0x00,
        ])
      )
    );

    buffer.writeDoubleBE(1.0000000000000004, 0);
    buffer.writeDoubleLE(1.0000000000000004, 8);
    ok(
      buffer.equals(
        new Uint8Array([
          0x3f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00,
          0x00, 0x00, 0x00, 0xf0, 0x3f,
        ])
      )
    );

    buffer.writeDoubleBE(-2, 0);
    buffer.writeDoubleLE(-2, 8);
    ok(
      buffer.equals(
        new Uint8Array([
          0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00, 0xc0,
        ])
      )
    );

    buffer.writeDoubleBE(1.7976931348623157e308, 0);
    buffer.writeDoubleLE(1.7976931348623157e308, 8);
    ok(
      buffer.equals(
        new Uint8Array([
          0x7f, 0xef, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff, 0xff, 0xef, 0x7f,
        ])
      )
    );

    buffer.writeDoubleBE(0 * -1, 0);
    buffer.writeDoubleLE(0 * -1, 8);
    ok(
      buffer.equals(
        new Uint8Array([
          0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00, 0x80,
        ])
      )
    );

    buffer.writeDoubleBE(Infinity, 0);
    buffer.writeDoubleLE(Infinity, 8);

    ok(
      buffer.equals(
        new Uint8Array([
          0x7f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0xf0, 0x7f,
        ])
      )
    );

    strictEqual(buffer.readDoubleBE(0), Infinity);
    strictEqual(buffer.readDoubleLE(8), Infinity);

    buffer.writeDoubleBE(-Infinity, 0);
    buffer.writeDoubleLE(-Infinity, 8);

    ok(
      buffer.equals(
        new Uint8Array([
          0xff, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0xf0, 0xff,
        ])
      )
    );

    strictEqual(buffer.readDoubleBE(0), -Infinity);
    strictEqual(buffer.readDoubleLE(8), -Infinity);

    buffer.writeDoubleBE(NaN, 0);
    buffer.writeDoubleLE(NaN, 8);

    // JS only knows a single NaN but there exist two platform specific
    // implementations. Therefore, allow both quiet and signalling NaNs.
    if (buffer[1] === 0xf7) {
      ok(
        buffer.equals(
          new Uint8Array([
            0x7f, 0xf7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xf7, 0x7f,
          ])
        )
      );
    } else {
      ok(
        buffer.equals(
          new Uint8Array([
            0x7f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0xf8, 0x7f,
          ])
        )
      );
    }

    ok(Number.isNaN(buffer.readDoubleBE(0)));
    ok(Number.isNaN(buffer.readDoubleLE(8)));

    // OOB in writeDouble{LE,BE} should throw.
    {
      const small = Buffer.allocUnsafe(1);

      ['writeDoubleLE', 'writeDoubleBE'].forEach((fn) => {
        // Verify that default offset works fine.
        buffer[fn](23, undefined);
        buffer[fn](23);

        throws(() => small[fn](11.11, 0), {
          code: 'ERR_BUFFER_OUT_OF_BOUNDS',
          name: 'RangeError',
          message: 'Attempt to access memory outside buffer bounds',
        });

        ['', '0', null, {}, [], () => {}, true, false].forEach((off) => {
          throws(() => small[fn](23, off), { name: 'TypeError' });
        });

        [Infinity, -1, 9].forEach((offset) => {
          throws(() => buffer[fn](23, offset), {
            name: 'RangeError',
          });
        });

        [NaN, 1.01].forEach((offset) => {
          throws(() => buffer[fn](42, offset), {
            name: 'RangeError',
          });
        });
      });
    }
  },
};

export const write = {
  test(ctrl, env, ctx) {
    [-1, 10].forEach((offset) => {
      throws(() => Buffer.alloc(9).write('foo', offset), {
        name: 'RangeError',
      });
    });

    const resultMap = new Map([
      ['utf8', Buffer.from([102, 111, 111, 0, 0, 0, 0, 0, 0])],
      ['ucs2', Buffer.from([102, 0, 111, 0, 111, 0, 0, 0, 0])],
      ['ascii', Buffer.from([102, 111, 111, 0, 0, 0, 0, 0, 0])],
      ['latin1', Buffer.from([102, 111, 111, 0, 0, 0, 0, 0, 0])],
      ['binary', Buffer.from([102, 111, 111, 0, 0, 0, 0, 0, 0])],
      ['utf16le', Buffer.from([102, 0, 111, 0, 111, 0, 0, 0, 0])],
      ['base64', Buffer.from([102, 111, 111, 0, 0, 0, 0, 0, 0])],
      ['base64url', Buffer.from([102, 111, 111, 0, 0, 0, 0, 0, 0])],
      ['hex', Buffer.from([102, 111, 111, 0, 0, 0, 0, 0, 0])],
    ]);

    // utf8, ucs2, ascii, latin1, utf16le
    const encodings = [
      'utf8',
      'utf-8',
      'ascii',
      'latin1',
      'binary',
      'ucs2',
      'ucs-2',
      'utf16le',
      'utf-16le',
    ];

    encodings
      .reduce((es, e) => es.concat(e, e.toUpperCase()), [])
      .forEach((encoding) => {
        const buf = Buffer.alloc(9);
        const len = Buffer.byteLength('foo', encoding);
        strictEqual(buf.write('foo', 0, len, encoding), len);

        if (encoding.includes('-')) encoding = encoding.replace('-', '');

        deepStrictEqual(buf, resultMap.get(encoding.toLowerCase()));
      });

    // base64
    ['base64', 'BASE64', 'base64url', 'BASE64URL'].forEach((encoding) => {
      const buf = Buffer.alloc(9);
      const len = Buffer.byteLength('Zm9v', encoding);

      strictEqual(buf.write('Zm9v', 0, len, encoding), len);
      deepStrictEqual(buf, resultMap.get(encoding.toLowerCase()));
    });

    // hex
    ['hex', 'HEX'].forEach((encoding) => {
      const buf = Buffer.alloc(9);
      const len = Buffer.byteLength('666f6f', encoding);

      strictEqual(buf.write('666f6f', 0, len, encoding), len);
      deepStrictEqual(buf, resultMap.get(encoding.toLowerCase()));
    });

    // Invalid encodings
    for (let i = 1; i < 10; i++) {
      const encoding = String(i).repeat(i);
      const error = {
        name: 'TypeError',
      };

      ok(!Buffer.isEncoding(encoding));
      throws(() => Buffer.alloc(9).write('foo', encoding), error);
    }

    // UCS-2 overflow CVE-2018-12115
    for (let i = 1; i < 4; i++) {
      // Allocate two Buffers sequentially off the pool. Run more than once in case
      // we hit the end of the pool and don't get sequential allocations
      const x = Buffer.allocUnsafe(4).fill(0);
      const y = Buffer.allocUnsafe(4).fill(1);
      // Should not write anything, pos 3 doesn't have enough room for a 16-bit char
      strictEqual(x.write('ыыыыыы', 3, 'ucs2'), 0);
      // CVE-2018-12115 experienced via buffer overrun to next block in the pool
      strictEqual(Buffer.compare(y, Buffer.alloc(4, 1)), 0);
    }

    // Should not write any data when there is no space for 16-bit chars
    const z = Buffer.alloc(4, 0);
    strictEqual(z.write('\u0001', 3, 'ucs2'), 0);
    strictEqual(Buffer.compare(z, Buffer.alloc(4, 0)), 0);
    // Make sure longer strings are written up to the buffer end.
    strictEqual(z.write('abcd', 2), 2);
    deepStrictEqual([...z], [0, 0, 0x61, 0x62]);

    // Large overrun could corrupt the process
    strictEqual(Buffer.alloc(4).write('ыыыыыы'.repeat(100), 3, 'utf16le'), 0);

    {
      // .write() does not affect the byte after the written-to slice of the Buffer.
      // Refs: https://github.com/nodejs/node/issues/26422
      const buf = Buffer.alloc(8);
      strictEqual(buf.write('ыы', 1, 'utf16le'), 4);
      deepStrictEqual([...buf], [0, 0x4b, 0x04, 0x4b, 0x04, 0, 0, 0]);
    }
  },
};

export const toString = {
  test(ctrl, env, ctx) {
    // utf8, ucs2, ascii, latin1, utf16le
    const encodings = [
      'utf8',
      'utf-8',
      'ucs2',
      'ucs-2',
      'ascii',
      'latin1',
      'binary',
      'utf16le',
      'utf-16le',
    ];

    encodings
      .reduce((es, e) => es.concat(e, e.toUpperCase()), [])
      .forEach((encoding) => {
        strictEqual(Buffer.from('foo', encoding).toString(encoding), 'foo');
      });

    // base64
    ['base64', 'BASE64'].forEach((encoding) => {
      strictEqual(Buffer.from('Zm9v', encoding).toString(encoding), 'Zm9v');
    });

    // hex
    ['hex', 'HEX'].forEach((encoding) => {
      strictEqual(Buffer.from('666f6f', encoding).toString(encoding), '666f6f');
    });

    // default utf-8 if undefined
    strictEqual(Buffer.from('utf-8').toString(), 'utf-8');

    const invalidEncodings = Array.from({ length: 10 }, (_, i) =>
      String(i + 1).repeat(i + 1)
    );
    // Invalid encodings
    for (const encoding of [...invalidEncodings, null]) {
      const error = {
        code: 'ERR_UNKNOWN_ENCODING',
        name: 'TypeError',
        message: `Unknown encoding: ${encoding}`,
      };
      ok(!Buffer.isEncoding(encoding));
      throws(() => Buffer.from('foo').toString(encoding), error);
    }
  },
};

export const toStringRangeError = {
  test(ctrl, env, ctx) {
    const len = 1422561062959;
    const message = {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
    };
    throws(() => Buffer(len).toString('utf8'), message);
    throws(() => SlowBuffer(len).toString('utf8'), message);
    throws(() => Buffer.alloc(len).toString('utf8'), message);
    throws(() => Buffer.allocUnsafe(len).toString('utf8'), message);
    throws(() => Buffer.allocUnsafeSlow(len).toString('utf8'), message);
  },
};

export const toStringRange = {
  test(ctrl, env, ctx) {
    const rangeBuffer = Buffer.from('abc');
    // If start >= buffer's length, empty string will be returned
    strictEqual(rangeBuffer.toString('ascii', 3), '');
    strictEqual(rangeBuffer.toString('ascii', +Infinity), '');
    strictEqual(rangeBuffer.toString('ascii', 3.14, 3), '');
    strictEqual(rangeBuffer.toString('ascii', 'Infinity', 3), '');

    // If end <= 0, empty string will be returned
    strictEqual(rangeBuffer.toString('ascii', 1, 0), '');
    strictEqual(rangeBuffer.toString('ascii', 1, -1.2), '');
    strictEqual(rangeBuffer.toString('ascii', 1, -100), '');
    strictEqual(rangeBuffer.toString('ascii', 1, -Infinity), '');

    // If start < 0, start will be taken as zero
    strictEqual(rangeBuffer.toString('ascii', -1, 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', -1.99, 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', -Infinity, 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', '-1', 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', '-1.99', 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', '-Infinity', 3), 'abc');

    // If start is an invalid integer, start will be taken as zero
    strictEqual(rangeBuffer.toString('ascii', 'node.js', 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', {}, 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', [], 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', NaN, 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', null, 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', undefined, 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', false, 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', '', 3), 'abc');

    // But, if start is an integer when coerced, then it will be coerced and used.
    strictEqual(rangeBuffer.toString('ascii', '-1', 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', '1', 3), 'bc');
    strictEqual(rangeBuffer.toString('ascii', '-Infinity', 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', '3', 3), '');
    strictEqual(rangeBuffer.toString('ascii', Number(3), 3), '');
    strictEqual(rangeBuffer.toString('ascii', '3.14', 3), '');
    strictEqual(rangeBuffer.toString('ascii', '1.99', 3), 'bc');
    strictEqual(rangeBuffer.toString('ascii', '-1.99', 3), 'abc');
    strictEqual(rangeBuffer.toString('ascii', 1.99, 3), 'bc');
    strictEqual(rangeBuffer.toString('ascii', true, 3), 'bc');

    // If end > buffer's length, end will be taken as buffer's length
    strictEqual(rangeBuffer.toString('ascii', 0, 5), 'abc');
    strictEqual(rangeBuffer.toString('ascii', 0, 6.99), 'abc');
    strictEqual(rangeBuffer.toString('ascii', 0, Infinity), 'abc');
    strictEqual(rangeBuffer.toString('ascii', 0, '5'), 'abc');
    strictEqual(rangeBuffer.toString('ascii', 0, '6.99'), 'abc');
    strictEqual(rangeBuffer.toString('ascii', 0, 'Infinity'), 'abc');

    // If end is an invalid integer, end will be taken as buffer's length
    strictEqual(rangeBuffer.toString('ascii', 0, 'node.js'), '');
    strictEqual(rangeBuffer.toString('ascii', 0, {}), '');
    strictEqual(rangeBuffer.toString('ascii', 0, NaN), '');
    strictEqual(rangeBuffer.toString('ascii', 0, undefined), 'abc');
    strictEqual(rangeBuffer.toString('ascii', 0), 'abc');
    strictEqual(rangeBuffer.toString('ascii', 0, null), '');
    strictEqual(rangeBuffer.toString('ascii', 0, []), '');
    strictEqual(rangeBuffer.toString('ascii', 0, false), '');
    strictEqual(rangeBuffer.toString('ascii', 0, ''), '');

    // But, if end is an integer when coerced, then it will be coerced and used.
    strictEqual(rangeBuffer.toString('ascii', 0, '-1'), '');
    strictEqual(rangeBuffer.toString('ascii', 0, '1'), 'a');
    strictEqual(rangeBuffer.toString('ascii', 0, '-Infinity'), '');
    strictEqual(rangeBuffer.toString('ascii', 0, '3'), 'abc');
    strictEqual(rangeBuffer.toString('ascii', 0, Number(3)), 'abc');
    strictEqual(rangeBuffer.toString('ascii', 0, '3.14'), 'abc');
    strictEqual(rangeBuffer.toString('ascii', 0, '1.99'), 'a');
    strictEqual(rangeBuffer.toString('ascii', 0, '-1.99'), '');
    strictEqual(rangeBuffer.toString('ascii', 0, 1.99), 'a');
    strictEqual(rangeBuffer.toString('ascii', 0, true), 'a');

    // Try toString() with an object as an encoding
    strictEqual(
      rangeBuffer.toString({
        toString: function () {
          return 'ascii';
        },
      }),
      'abc'
    );

    // Try toString() with 0 and null as the encoding
    throws(
      () => {
        rangeBuffer.toString(0, 1, 2);
      },
      {
        name: 'TypeError',
      },
      'toString() with 0 and null as the encoding should have thrown'
    );

    throws(
      () => {
        rangeBuffer.toString(null, 1, 2);
      },
      {
        name: 'TypeError',
      },
      'toString() with null encoding should have thrown'
    );
  },
};

export const inspect = {
  // test-buffer-inspect.js
  async test(ctrl, env, ctx) {
    let b = Buffer.allocUnsafe(60);
    b.fill('0123456789'.repeat(6));

    let s = buffer.SlowBuffer(60);
    s.fill('0123456789'.repeat(6));

    let expected =
      '<Buffer 30 31 32 33 34 35 36 37 38 39 30 31 32 33 34 35 36 37 38 39 30 31 32 33 34 35 36 37 38 39 30 31 32 33 34 35 36 37 38 39 30 31 32 33 34 35 36 37 38 39 ... 10 more bytes>';

    strictEqual(util.inspect(b), expected);
    strictEqual(util.inspect(s), expected);

    b = Buffer.allocUnsafe(2);
    b.fill('12');

    s = buffer.SlowBuffer(2);
    s.fill('12');

    expected = '<Buffer 31 32>';

    strictEqual(util.inspect(b), expected);
    strictEqual(util.inspect(s), expected);

    b.inspect = undefined;
    b.prop = new Uint8Array(0);
    strictEqual(
      util.inspect(b),
      '<Buffer 31 32, inspect: undefined, prop: Uint8Array(0) []>'
    );

    b = Buffer.alloc(0);
    b.prop = 123;

    strictEqual(util.inspect(b), '<Buffer prop: 123>');
  },
};

export const isAsciiTest = {
  test(ctrl, env, ctx) {
    const encoder = new TextEncoder();
    strictEqual(isAscii(encoder.encode('hello')), true);
    strictEqual(isAscii(encoder.encode('ğ')), false);
    strictEqual(isAscii(Buffer.from([])), true);

    [
      undefined,
      '',
      'hello',
      false,
      true,
      0,
      1,
      0n,
      1n,
      Symbol(),
      () => {},
      {},
      [],
      null,
    ].forEach((input) => {
      throws(() => isAscii(input));
    });
  },
};

export const isUtf8Test = {
  test(ctrl, env, ctx) {
    const encoder = new TextEncoder();

    strictEqual(isUtf8(encoder.encode('hello')), true);
    strictEqual(isUtf8(encoder.encode('ğ')), true);
    strictEqual(isUtf8(Buffer.from([])), true);

    // Taken from test/fixtures/wpt/encoding/textdecoder-fatal.any.js
    [
      [0xff], // 'invalid code'
      [0xc0], // 'ends early'
      [0xe0], // 'ends early 2'
      [0xc0, 0x00], // 'invalid trail'
      [0xc0, 0xc0], // 'invalid trail 2'
      [0xe0, 0x00], // 'invalid trail 3'
      [0xe0, 0xc0], // 'invalid trail 4'
      [0xe0, 0x80, 0x00], // 'invalid trail 5'
      [0xe0, 0x80, 0xc0], // 'invalid trail 6'
      [0xfc, 0x80, 0x80, 0x80, 0x80, 0x80], // '> 0x10FFFF'
      [0xfe, 0x80, 0x80, 0x80, 0x80, 0x80], // 'obsolete lead byte'

      // Overlong encodings
      [0xc0, 0x80], // 'overlong U+0000 - 2 bytes'
      [0xe0, 0x80, 0x80], // 'overlong U+0000 - 3 bytes'
      [0xf0, 0x80, 0x80, 0x80], // 'overlong U+0000 - 4 bytes'
      [0xf8, 0x80, 0x80, 0x80, 0x80], // 'overlong U+0000 - 5 bytes'
      [0xfc, 0x80, 0x80, 0x80, 0x80, 0x80], // 'overlong U+0000 - 6 bytes'

      [0xc1, 0xbf], // 'overlong U+007F - 2 bytes'
      [0xe0, 0x81, 0xbf], // 'overlong U+007F - 3 bytes'
      [0xf0, 0x80, 0x81, 0xbf], // 'overlong U+007F - 4 bytes'
      [0xf8, 0x80, 0x80, 0x81, 0xbf], // 'overlong U+007F - 5 bytes'
      [0xfc, 0x80, 0x80, 0x80, 0x81, 0xbf], // 'overlong U+007F - 6 bytes'

      [0xe0, 0x9f, 0xbf], // 'overlong U+07FF - 3 bytes'
      [0xf0, 0x80, 0x9f, 0xbf], // 'overlong U+07FF - 4 bytes'
      [0xf8, 0x80, 0x80, 0x9f, 0xbf], // 'overlong U+07FF - 5 bytes'
      [0xfc, 0x80, 0x80, 0x80, 0x9f, 0xbf], // 'overlong U+07FF - 6 bytes'

      [0xf0, 0x8f, 0xbf, 0xbf], // 'overlong U+FFFF - 4 bytes'
      [0xf8, 0x80, 0x8f, 0xbf, 0xbf], // 'overlong U+FFFF - 5 bytes'
      [0xfc, 0x80, 0x80, 0x8f, 0xbf, 0xbf], // 'overlong U+FFFF - 6 bytes'

      [0xf8, 0x84, 0x8f, 0xbf, 0xbf], // 'overlong U+10FFFF - 5 bytes'
      [0xfc, 0x80, 0x84, 0x8f, 0xbf, 0xbf], // 'overlong U+10FFFF - 6 bytes'

      // UTF-16 surrogates encoded as code points in UTF-8
      [0xed, 0xa0, 0x80], // 'lead surrogate'
      [0xed, 0xb0, 0x80], // 'trail surrogate'
      [0xed, 0xa0, 0x80, 0xed, 0xb0, 0x80], // 'surrogate pair'
    ].forEach((input) => {
      strictEqual(isUtf8(Buffer.from(input)), false);
    });

    [null, undefined, 'hello', true, false].forEach((input) => {
      throws(() => isUtf8(input));
    });
  },
};

// Adapted from test/parallel/test-icu-transcode.js
export const transcodeTest = {
  test(ctrl, env, ctx) {
    const orig = Buffer.from('těst ☕', 'utf8');
    const tests = {
      latin1: [0x74, 0x3f, 0x73, 0x74, 0x20, 0x3f],
      ascii: [0x74, 0x3f, 0x73, 0x74, 0x20, 0x3f],
      ucs2: [
        0x74, 0x00, 0x1b, 0x01, 0x73, 0x00, 0x74, 0x00, 0x20, 0x00, 0x15, 0x26,
      ],
    };

    for (const test in tests) {
      const dest = transcode(orig, 'utf8', test);
      strictEqual(
        dest.length,
        tests[test].length,
        `utf8->${test} length (${dest.length}, ${tests[test].length})`
      );
      for (let n = 0; n < tests[test].length; n++) {
        strictEqual(dest[n], tests[test][n], `utf8->${test} char ${n}`);
      }
    }

    {
      const dest = transcode(Buffer.from(tests.ucs2), 'ucs2', 'utf8');
      strictEqual(dest.toString(), orig.toString());
    }

    // Test utf16le to ascii/latin1 output length
    {
      const input = Buffer.from('AAA', 'utf16le');
      strictEqual(input.length, 6);
      const asciiOutput = transcode(input, 'utf16le', 'ascii');
      strictEqual(asciiOutput.length, 3);
      deepStrictEqual(asciiOutput, Buffer.from('AAA', 'ascii'));
      const latin1Output = transcode(input, 'utf16le', 'latin1');
      strictEqual(latin1Output.length, 3);
      deepStrictEqual(latin1Output, Buffer.from('AAA', 'latin1'));
    }

    {
      const utf8 = Buffer.from('€'.repeat(4000), 'utf8');
      const ucs2 = Buffer.from('€'.repeat(4000), 'ucs2');
      const utf8_to_ucs2 = transcode(utf8, 'utf8', 'ucs2');
      const ucs2_to_utf8 = transcode(ucs2, 'ucs2', 'utf8');
      deepStrictEqual(utf8, ucs2_to_utf8);
      deepStrictEqual(ucs2, utf8_to_ucs2);
      strictEqual(ucs2_to_utf8.toString('utf8'), utf8_to_ucs2.toString('ucs2'));
    }

    {
      deepStrictEqual(
        transcode(Buffer.from('hi', 'ascii'), 'ascii', 'utf16le'),
        Buffer.from('hi', 'utf16le')
      );
      deepStrictEqual(
        transcode(Buffer.from('hi', 'latin1'), 'latin1', 'utf16le'),
        Buffer.from('hi', 'utf16le')
      );
      deepStrictEqual(
        transcode(Buffer.from('hä', 'latin1'), 'latin1', 'utf16le'),
        Buffer.from('hä', 'utf16le')
      );
    }

    {
      const dest = transcode(new Uint8Array(), 'utf8', 'latin1');
      strictEqual(dest.length, 0);
    }

    // Test that Uint8Array arguments are okay.
    {
      const uint8array = new Uint8Array(Buffer.from('hä', 'latin1'));
      deepStrictEqual(
        transcode(uint8array, 'latin1', 'utf16le'),
        Buffer.from('hä', 'utf16le')
      );
    }

    // Invalid arguments should fail
    throws(() => transcode(null, 'utf8', 'ascii'));
    throws(() => transcode(Buffer.from('a'), 'b', 'utf8'));
    throws(() => transcode(Buffer.from('a'), 'uf8', 'b'));

    // Throws error for buffer bigger than 128mb.
    {
      const ISOLATE_MAX_SIZE = 134217728;
      const val = Buffer.from('a'.repeat(ISOLATE_MAX_SIZE));

      throws(() => transcode(val, 'utf16le', 'utf8'));
      throws(() => transcode(val, 'latin1', 'utf16le'));
    }

    // Make sure same fromEncoding and toEncoding results in copy.
    {
      const original = Buffer.from('a');
      const copied_value = transcode(original, 'utf8', 'utf8');
      // Let's detach the copied_value
      const _ = copied_value.buffer.transfer();
      ok(copied_value.buffer.detached);
      ok(!original.buffer.detached);
    }

    // Same encoding types should return in a value that replaces
    // invalid characters with replacement characters.
    deepStrictEqual(
      transcode(Buffer.from([0x80]), 'utf8', 'utf8'),
      Buffer.from([0xef, 0xbf, 0xbd])
    );
  },
};

// TranscodeFromUTF16 should not over-allocate the output buffer.
// Regression test for a bug where `limit * sizeof(char16_t)` doubled the
// allocation and `destPtr.size()` passed char16_t element count instead of
// byte count to ucnv_fromUChars.
export const transcodeFromUTF16BufferSizeTest = {
  test() {
    const utf16 = Buffer.from('Hello', 'utf16le');
    const latin1 = transcode(utf16, 'utf16le', 'latin1');
    strictEqual(latin1.length, 5);
    strictEqual(latin1.buffer.byteLength, latin1.length);
  },
};

// Invalid UTF-8 input to transcode('utf8', 'utf16le') should produce
// "Unable to transcode buffer", not an internal assertion mismatch.
// Regression test for a bug where JSG_REQUIRE(actual == expected) threw
// before the `if (actual == 0) return kj::none` path could be reached.
export const transcodeUTF8ToUTF16InvalidInputTest = {
  test() {
    throws(
      () =>
        transcode(
          Buffer.from([0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x80]),
          'utf8',
          'utf16le'
        ),
      { message: /Unable to transcode buffer/ }
    );
  },
};

// TranscodeFromUTF16 should reject odd-byte UTF-16LE input, matching
// the guard that TranscodeUTF8FromUTF16 already has.
// Regression test for a bug where `source.size() / sizeof(char16_t)`
// silently dropped the trailing byte.
export const transcodeFromUTF16OddByteInputTest = {
  test() {
    const oddInput = Buffer.from([0x41, 0x00, 0x42]);
    throws(() => transcode(oddInput, 'utf16le', 'utf8'));
    throws(() => transcode(oddInput, 'utf16le', 'latin1'));
    throws(() => transcode(oddInput, 'utf16le', 'ascii'));
  },
};

// Tests are taken from Node.js
// https://github.com/nodejs/node/blob/a4f609fa/test/parallel/test-file.js
export const fileTest = {
  test() {
    throws(() => new File(), TypeError);
    throws(() => new File([]), TypeError);
    throws(() => File.prototype.name, TypeError);
    throws(() => File.prototype.lastModified, TypeError);

    {
      const keys = Object.keys(File.prototype).sort();
      deepStrictEqual(keys, ['lastModified', 'name']);
    }

    {
      const file = new File([], 'dummy.txt.exe');
      strictEqual(file.name, 'dummy.txt.exe');
      strictEqual(file.size, 0);
      strictEqual(typeof file.lastModified, 'number');
      ok(file.lastModified <= Date.now());
    }

    {
      const toPrimitive = {
        [Symbol.toPrimitive]() {
          return 'NaN';
        },
      };

      const invalidLastModified = [null, 'string', false, toPrimitive];

      for (const lastModified of invalidLastModified) {
        const file = new File([], '', { lastModified });
        strictEqual(file.lastModified, 0);
      }
    }

    {
      const file = new File([], '', { lastModified: undefined });
      notStrictEqual(file.lastModified, 0);
    }

    {
      const toPrimitive = {
        [Symbol.toPrimitive]() {
          throw new TypeError('boom');
        },
      };

      const throwValues = [BigInt(3n), toPrimitive];

      for (const lastModified of throwValues) {
        throws(() => new File([], '', { lastModified }), TypeError);
      }
    }

    {
      const valid = [
        {
          [Symbol.toPrimitive]() {
            return 10;
          },
        },
        new Number(10),
        10,
      ];

      for (const lastModified of valid) {
        strictEqual(new File([], '', { lastModified }).lastModified, 10);
      }
    }

    {
      function MyClass() {}
      MyClass.prototype.lastModified = 10;

      const file = new File([], '', new MyClass());
      strictEqual(file.lastModified, 10);
    }

    {
      let counter = 0;
      new File([], '', {
        get lastModified() {
          counter++;
          return 10;
        },
      });
      strictEqual(counter, 1);
    }
  },
};

// Ref: https://github.com/cloudflare/workerd/issues/2538
export const sliceOffsetLimits = {
  test() {
    // Make sure the second parameter represents the "end" index, not length.
    strictEqual(Buffer.from('abcd').utf8Slice(2, 3).toString(), 'c');
    // Make sure to handle (end < start) edge case.
    strictEqual(Buffer.from('abcd').utf8Slice(1, 0).toString(), '');
  },
};

// Ref: https://github.com/unjs/unenv/pull/325
// Without `.bind(globalThis)` the following tests fail.
export const invalidThisTests = {
  async test() {
    const bufferModule = await import('node:buffer');
    strictEqual(bufferModule.btoa('hello'), 'aGVsbG8=');
    strictEqual(bufferModule.atob('aGVsbG8='), 'hello');
    ok(new bufferModule.File([], 'file'));
    ok(new bufferModule.Blob([]));
  },
};
