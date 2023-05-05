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

'use strict';

import * as assert from 'node:assert';
import * as crypto from 'node:crypto';

function deferredPromise() {
  let resolve, reject;
  const promise = new Promise((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return {
    promise,
    resolve,
    reject,
  }
}

function mustNotCall(){return () => {};}

async function runPBKDF2(password, salt, iterations, keylen, hash) {
  const syncResult =
    crypto.pbkdf2Sync(password, salt, iterations, keylen, hash);

  const p = deferredPromise();
  crypto.pbkdf2(password, salt, iterations, keylen, hash,
                (err, asyncResult) => {
                  assert.deepStrictEqual(asyncResult, syncResult);
                  p.resolve();
                });
  await p.promise;

  return syncResult;
}

async function testPBKDF2(password, salt, iterations, keylen, expected, encoding) {
  const actual = await runPBKDF2(password, salt, iterations, keylen, 'sha256');
  assert.strictEqual(actual.toString(encoding || 'latin1'), expected);
}

//
// Test PBKDF2 with RFC 6070 test vectors (except #4)
//

export const pbkdf2_correctness_tests = {
  async test(ctrl, env, ctx) {
    await testPBKDF2('password', 'salt', 1, 20,
               '\x12\x0f\xb6\xcf\xfc\xf8\xb3\x2c\x43\xe7\x22\x52' +
               '\x56\xc4\xf8\x37\xa8\x65\x48\xc9');

    await testPBKDF2('password', 'salt', 2, 20,
               '\xae\x4d\x0c\x95\xaf\x6b\x46\xd3\x2d\x0a\xdf\xf9' +
               '\x28\xf0\x6d\xd0\x2a\x30\x3f\x8e');

    await testPBKDF2('password', 'salt', 4096, 20,
               '\xc5\xe4\x78\xd5\x92\x88\xc8\x41\xaa\x53\x0d\xb6' +
               '\x84\x5c\x4c\x8d\x96\x28\x93\xa0');

    await testPBKDF2('passwordPASSWORDpassword',
               'saltSALTsaltSALTsaltSALTsaltSALTsalt',
               4096,
               25,
               '\x34\x8c\x89\xdb\xcb\xd3\x2b\x2f\x32\xd8\x14\xb8\x11' +
               '\x6e\x84\xcf\x2b\x17\x34\x7e\xbc\x18\x00\x18\x1c');

    await testPBKDF2('pass\0word', 'sa\0lt', 4096, 16,
               '\x89\xb6\x9d\x05\x16\xf8\x29\x89\x3c\x69\x62\x26\x65' +
               '\x0a\x86\x87');

    await testPBKDF2('password', 'salt', 32, 32,
               '64c486c55d30d4c5a079b8823b7d7cb37ff0556f537da8410233bcec330ed956',
               'hex');
  }
}

export const pbkdf2_no_callback_test = {
  test(ctrl, env, ctx) {
    // Error path should not leak memory (check with valgrind).
    assert.throws(
      () => crypto.pbkdf2('password', 'salt', 1, 20, 'sha1'),
      {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError'
      }
    );
  }
}

export const pbkdf2_out_of_range_tests = {
  test(ctrl, env, ctx) {
    for (const iterations of [-1, 0, 2147483648]) {
      assert.throws(
        () => crypto.pbkdf2Sync('password', 'salt', iterations, 20, 'sha1'),
        {
          code: 'ERR_OUT_OF_RANGE',
          name: 'RangeError',
        }
      );
    }

    ['str', null, undefined, [], {}].forEach((notNumber) => {
      assert.throws(
        () => {
          crypto.pbkdf2Sync('password', 'salt', 1, notNumber, 'sha256');
        }, {
          code: 'ERR_INVALID_ARG_TYPE',
          name: 'TypeError',
        });
    });

    [Infinity, -Infinity, NaN].forEach((input) => {
      assert.throws(
        () => {
          crypto.pbkdf2('password', 'salt', 1, input, 'sha256',
                        mustNotCall());
        }, {
          code: 'ERR_OUT_OF_RANGE',
          name: 'RangeError',
          message: 'The value of "keylen" is out of range. It ' +
                   `must be an integer. Received ${input}`
        });
    });

    [-1, 2147483648, 4294967296].forEach((input) => {
      assert.throws(
        () => {
          crypto.pbkdf2('password', 'salt', 1, input, 'sha256',
                        mustNotCall());
        }, {
          code: 'ERR_OUT_OF_RANGE',
          name: 'RangeError',
        });
    });
  }
}

export const empty_pwd_test = {
  async test(ctrl, env, ctx) {
    // Should not get FATAL ERROR with empty password and salt
    // https://github.com/nodejs/node/issues/8571
    const p = deferredPromise();
    crypto.pbkdf2('', '', 1, 32, 'sha256', (err, prime) => {
      p.resolve();
    });
    await p.promise;
  }
}

export const invalid_arg_tests = {
  test(ctrl, env, ctx) {
    assert.throws(
      () => crypto.pbkdf2('password', 'salt', 8, 8, mustNotCall()),
      {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
        message: 'The "digest" argument must be of type string. ' +
                 'Received undefined'
      });

    assert.throws(
      () => crypto.pbkdf2Sync('password', 'salt', 8, 8),
      {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
        message: 'The "digest" argument must be of type string. ' +
                 'Received undefined'
      });

    assert.throws(
      () => crypto.pbkdf2Sync('password', 'salt', 8, 8, null),
      {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
        message: 'The "digest" argument must be of type string. ' +
                 'Received null'
      });
    [1, {}, [], true, undefined, null].forEach((input) => {
      assert.throws(
        () => crypto.pbkdf2(input, 'salt', 8, 8, 'sha256', mustNotCall()),
        {
          code: 'ERR_INVALID_ARG_TYPE',
          name: 'TypeError',
        }
      );

      assert.throws(
        () => crypto.pbkdf2('pass', input, 8, 8, 'sha256', mustNotCall()),
        {
          code: 'ERR_INVALID_ARG_TYPE',
          name: 'TypeError',
        }
      );

      assert.throws(
        () => crypto.pbkdf2Sync(input, 'salt', 8, 8, 'sha256'),
        {
          code: 'ERR_INVALID_ARG_TYPE',
          name: 'TypeError',
        }
      );

      assert.throws(
        () => crypto.pbkdf2Sync('pass', input, 8, 8, 'sha256'),
        {
          code: 'ERR_INVALID_ARG_TYPE',
          name: 'TypeError',
        }
      );
    });

    ['test', {}, [], true, undefined, null].forEach((i) => {
      assert.throws(
        () => crypto.pbkdf2('pass', 'salt', i, 8, 'sha256', mustNotCall()),
        {
          code: 'ERR_INVALID_ARG_TYPE',
          name: 'TypeError',
        }
      );

      assert.throws(
        () => crypto.pbkdf2Sync('pass', 'salt', i, 8, 'sha256'),
        {
          code: 'ERR_INVALID_ARG_TYPE',
          name: 'TypeError',
        }
      );
    });
  }
}

export const TypedArray_tests = {
  async test(ctrl, env, ctx) {
    // Any TypedArray should work for password and salt.
    for (const SomeArray of [Uint8Array, Uint16Array, Uint32Array, Float32Array,
                             Float64Array, ArrayBuffer]) {
      await runPBKDF2(new SomeArray(10), 'salt', 8, 8, 'sha256');
      await runPBKDF2('pass', new SomeArray(10), 8, 8, 'sha256');
    }
  }
}

export const invalid_digest_tests = {
  async test(ctrl, env, ctx) {
    {
      const p = deferredPromise();
      crypto.pbkdf2('pass', 'salt', 8, 8, 'md55',
        (err, prime) => {
        if (err) return p.reject(err);
      });
      await assert.rejects(p.promise);
    }

    assert.throws(
      () => crypto.pbkdf2Sync('pass', 'salt', 8, 8, 'md55'),
      {
        name: 'TypeError',
      }
    );

    // TODO(soon): Enable this once crypto.getHashes() is available. Note that shake* is not
    // supported by boringssl so there's no need to filter it out, but we may want to filter other
    // functions.
    // const kNotPBKDF2Supported = ['shake128', 'shake256'];
    // crypto.getHashes()
    //   .filter((hash) => !kNotPBKDF2Supported.includes(hash))
    //   .forEach((hash) => {
    //     runPBKDF2(new Uint8Array(10), 'salt', 8, 8, hash);
    //   });

    {
      // This should not crash.
      assert.throws(
        () => crypto.pbkdf2Sync('1', '2', 1, 1, '%'),
        {
          name: 'TypeError',
        }
      );
    }
  }
}
