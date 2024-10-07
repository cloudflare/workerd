// Copyright (c) 2017-2023 Cloudflare, Inc.
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

'use strict';

import { Buffer } from 'node:buffer';
import * as assert from 'node:assert';
import * as crypto from 'node:crypto';
import * as stream from 'node:stream';

let cryptoType;
let digest;

export const hash_correctness_tests = {
  async test(ctrl, env, ctx) {
    const a1 = crypto.createHash('sha1').update('Test123').digest('hex');
    const a2 = crypto.createHash('sha256').update('Test123').digest('base64');
    const a3 = crypto.createHash('sha512').update('Test123').digest(); // buffer
    const a4 = crypto.createHash('sha1').update('Test123').digest('buffer');

    // stream interface
    let a5 = crypto.createHash('sha512');
    a5.end('Test123');
    a5 = a5.read();

    let a6 = crypto.createHash('sha512');
    a6.write('Te');
    a6.write('st');
    a6.write('123');
    a6.end();
    a6 = a6.read();

    let a7 = crypto.createHash('sha512');
    a7.end();
    a7 = a7.read();

    let a8 = crypto.createHash('sha512');
    a8.write('');
    a8.end();
    a8 = a8.read();

    cryptoType = 'md5';
    digest = 'latin1';
    const a0 = crypto.createHash(cryptoType).update('Test123').digest(digest);
    assert.strictEqual(
      a0,
      'h\u00ea\u00cb\u0097\u00d8o\fF!\u00fa+\u000e\u0017\u00ca\u00bd\u008c',
      `${cryptoType} with ${digest} digest failed to evaluate to expected hash`
    );

    cryptoType = 'md5';
    digest = 'hex';
    assert.strictEqual(
      a1,
      '8308651804facb7b9af8ffc53a33a22d6a1c8ac2',
      `${cryptoType} with ${digest} digest failed to evaluate to expected hash`
    );
    cryptoType = 'sha256';
    digest = 'base64';
    assert.strictEqual(
      a2,
      '2bX1jws4GYKTlxhloUB09Z66PoJZW+y+hq5R8dnx9l4=',
      `${cryptoType} with ${digest} digest failed to evaluate to expected hash`
    );

    cryptoType = 'sha512';
    digest = 'latin1';
    assert.deepStrictEqual(
      a3,
      Buffer.from(
        "\u00c1(4\u00f1\u0003\u001fd\u0097!O'\u00d4C/&Qz\u00d4" +
          '\u0094\u0015l\u00b8\u008dQ+\u00db\u001d\u00c4\u00b5}\u00b2' +
          '\u00d6\u0092\u00a3\u00df\u00a2i\u00a1\u009b\n\n*\u000f' +
          '\u00d7\u00d6\u00a2\u00a8\u0085\u00e3<\u0083\u009c\u0093' +
          "\u00c2\u0006\u00da0\u00a1\u00879(G\u00ed'",
        'latin1'
      ),
      `${cryptoType} with ${digest} digest failed to evaluate to expected hash`
    );

    cryptoType = 'sha1';
    digest = 'hex';
    assert.deepStrictEqual(
      a4,
      Buffer.from('8308651804facb7b9af8ffc53a33a22d6a1c8ac2', 'hex'),
      `${cryptoType} with ${digest} digest failed to evaluate to expected hash`
    );

    cryptoType = 'sha1';
    digest = 'hex';
    assert.deepStrictEqual(
      a4,
      Buffer.from('8308651804FACB7B9AF8FFC53A33A22D6A1C8AC2', 'hex'),
      `${cryptoType} with ${digest} digest failed to evaluate to expected hash`
    );

    // Stream interface should produce the same result.
    assert.deepStrictEqual(a5, a3);
    assert.deepStrictEqual(a6, a3);
    assert.notStrictEqual(a7, undefined);
    assert.notStrictEqual(a8, undefined);

    // Test multiple updates to same hash
    const h1 = crypto.createHash('sha1').update('Test123').digest('hex');
    const h2 = crypto
      .createHash('sha1')
      .update('Test')
      .update('123')
      .digest('hex');
    assert.strictEqual(h1, h2);
  },
};

export const hash_error_test = {
  async test(ctrl, env, ctx) {
    // Issue https://github.com/nodejs/node-v0.x-archive/issues/2227: unknown digest
    // method should throw an error.
    assert.throws(function () {
      crypto.createHash('xyzzy');
    }, /Digest method not supported/);

    // Issue https://github.com/nodejs/node/issues/9819: throwing encoding used to
    // segfault.
    assert.throws(
      () =>
        crypto.createHash('sha256').digest({
          toString: () => {
            throw new Error('boom');
          },
        }),
      {
        name: 'Error',
        message: 'boom',
      }
    );

    // Issue https://github.com/nodejs/node/issues/25487: error message for invalid
    // arg type to update method should include all possible types
    assert.throws(() => crypto.createHash('sha256').update(), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
    });

    // Default UTF-8 encoding
    const hutf8 = crypto
      .createHash('sha512')
      .update('УТФ-8 text')
      .digest('hex');
    assert.strictEqual(
      hutf8,
      '4b21bbd1a68e690a730ddcb5a8bc94ead9879ffe82580767ad7ec6fa8ba2dea6' +
        '43a821af66afa9a45b6a78c712fecf0e56dc7f43aef4bcfc8eb5b4d8dca6ea5b'
    );

    assert.notStrictEqual(
      hutf8,
      crypto.createHash('sha512').update('УТФ-8 text', 'latin1').digest('hex')
    );

    const h3 = crypto.createHash('sha256');
    h3.digest();

    assert.throws(() => h3.digest(), {
      code: 'ERR_CRYPTO_HASH_FINALIZED',
      name: 'Error',
    });

    assert.throws(() => h3.update('foo'), {
      code: 'ERR_CRYPTO_HASH_FINALIZED',
      name: 'Error',
    });

    assert.strictEqual(
      crypto.createHash('sha256').update('test').digest('ucs2'),
      crypto.createHash('sha256').update('test').digest().toString('ucs2')
    );

    assert.throws(() => crypto.createHash(), {
      code: 'ERR_INVALID_ARG_TYPE',
      name: 'TypeError',
      message:
        'The "algorithm" argument must be of type string. ' +
        'Received undefined',
    });

    {
      const Hash = crypto.Hash;
      const instance = crypto.Hash('sha256');
      assert.ok(
        instance instanceof Hash,
        'Hash is expected to return a new instance' +
          ' when called without `new`'
      );
    }

    // shake*-based tests for XOF hash function are not supported in FIPS and have been removed.
    {
      // Non-XOF hash functions should accept valid outputLength options as well.
      assert.strictEqual(
        crypto.createHash('sha224', { outputLength: 28 }).digest('hex'),
        'd14a028c2a3a2bc9476102bb288234c4' + '15a2b01f828ea62ac5b3e42f'
      );

      // Passing invalid sizes should throw during creation.
      assert.throws(
        () => {
          crypto.createHash('sha256', { outputLength: 28 });
        },
        {
          name: 'Error',
        }
      );

      for (const outputLength of [null, {}, 'foo', false]) {
        assert.throws(() => crypto.createHash('sha256', { outputLength }), {
          code: 'ERR_INVALID_ARG_TYPE',
        });
      }

      for (const outputLength of [-1, 0.5, Infinity, 2 ** 90]) {
        assert.throws(() => crypto.createHash('sha256', { outputLength }), {
          code: 'ERR_OUT_OF_RANGE',
        });
      }
    }
  },
};

export const hash_copy_test = {
  async test(ctrl, env, ctx) {
    const h = crypto.createHash('sha512');
    h.digest();
    assert.throws(() => h.copy(), { code: 'ERR_CRYPTO_HASH_FINALIZED' });
    assert.throws(() => h.digest(), { code: 'ERR_CRYPTO_HASH_FINALIZED' });

    const a = crypto.createHash('sha512').update('abc');
    const b = a.copy();
    const c = b.copy().update('def');
    const d = crypto.createHash('sha512').update('abcdef');
    assert.strictEqual(a.digest('hex'), b.digest('hex'));
    assert.strictEqual(c.digest('hex'), d.digest('hex'));
  },
};

export const hash_pipe_test = {
  async test(ctrl, env, ctx) {
    const p = Promise.withResolvers();

    const s = new stream.PassThrough();
    const h = crypto.createHash('sha512');
    const expect =
      'fba055c6fd0c5b6645407749ed7a8b41' +
      'b8f629f2163c3ca3701d864adabda1f8' +
      '93c37bf82b22fdd151ba8e357f611da4' +
      '88a74b6a5525dd9b69554c6ce5138ad7';

    s.pipe(h)
      .on('data', function (c) {
        // Calling digest() after piping into a stream with SHA3 should not cause
        // a segmentation fault, see https://github.com/nodejs/node/issues/28245.
        if (c !== expect || h.digest('hex') !== expect) {
          p.reject('Unexpected value for stream-based hash');
        }
        p.resolve();
      })
      .setEncoding('hex');

    s.end('aoeu');
    await p.promise;
  },
};
