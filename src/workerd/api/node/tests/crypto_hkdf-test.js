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

import { Buffer, kMaxLength } from 'node:buffer';
import * as assert from 'node:assert';

import {
  // createSecretKey,
  hkdf,
  hkdfSync,
  getHashes,
} from 'node:crypto';

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
  };
}

export const hkdf_error_tests = {
  async test(ctrl, env, ctx) {
    assert.throws(() => hkdf(), {
      code: 'ERR_INVALID_ARG_TYPE',
      message: /The "digest" argument must be of type string/,
    });

    [1, {}, [], false, Infinity].forEach((i) => {
      assert.throws(() => hkdf(i, 'a'), {
        code: 'ERR_INVALID_ARG_TYPE',
        message: /^The "digest" argument must be of type string/,
      });
      assert.throws(() => hkdfSync(i, 'a'), {
        code: 'ERR_INVALID_ARG_TYPE',
        message: /^The "digest" argument must be of type string/,
      });
    });

    [1, {}, [], false, Infinity].forEach((i) => {
      assert.throws(() => hkdf('sha256', i), {
        code: 'ERR_INVALID_ARG_TYPE',
        message: /^The "ikm" argument must be /,
      });
      assert.throws(() => hkdfSync('sha256', i), {
        code: 'ERR_INVALID_ARG_TYPE',
        message: /^The "ikm" argument must be /,
      });
    });

    [1, {}, [], false, Infinity].forEach((i) => {
      assert.throws(() => hkdf('sha256', 'secret', i), {
        code: 'ERR_INVALID_ARG_TYPE',
        message: /^The "salt" argument must be /,
      });
      assert.throws(() => hkdfSync('sha256', 'secret', i), {
        code: 'ERR_INVALID_ARG_TYPE',
        message: /^The "salt" argument must be /,
      });
    });

    [1, {}, [], false, Infinity].forEach((i) => {
      assert.throws(() => hkdf('sha256', 'secret', 'salt', i), {
        code: 'ERR_INVALID_ARG_TYPE',
        message: /^The "info" argument must be /,
      });
      assert.throws(() => hkdfSync('sha256', 'secret', 'salt', i), {
        code: 'ERR_INVALID_ARG_TYPE',
        message: /^The "info" argument must be /,
      });
    });

    ['test', {}, [], false].forEach((i) => {
      assert.throws(() => hkdf('sha256', 'secret', 'salt', 'info', i), {
        code: 'ERR_INVALID_ARG_TYPE',
        message: /^The "length" argument must be of type number/,
      });
      assert.throws(() => hkdfSync('sha256', 'secret', 'salt', 'info', i), {
        code: 'ERR_INVALID_ARG_TYPE',
        message: /^The "length" argument must be of type number/,
      });
    });

    assert.throws(() => hkdf('sha256', 'secret', 'salt', 'info', -1), {
      code: 'ERR_OUT_OF_RANGE',
    });
    assert.throws(() => hkdfSync('sha256', 'secret', 'salt', 'info', -1), {
      code: 'ERR_OUT_OF_RANGE',
    });
    assert.throws(
      () => hkdf('sha256', 'secret', 'salt', 'info', kMaxLength + 1),
      {
        code: 'ERR_OUT_OF_RANGE',
      }
    );
    assert.throws(
      () => hkdfSync('sha256', 'secret', 'salt', 'info', kMaxLength + 1),
      {
        code: 'ERR_OUT_OF_RANGE',
      }
    );

    {
      const p = deferredPromise();
      hkdf('unknown', 'a', '', '', 10, (err, asyncResult) => {
        if (err) {
          return p.reject(err);
        }
        p.resolve();
      });
      await assert.rejects(p.promise);
    }
    assert.throws(() => hkdfSync('unknown', 'a', '', '', 10), {
      name: 'TypeError',
    });

    assert.throws(() => hkdf('unknown', 'a', '', Buffer.alloc(1025), 10), {
      code: 'ERR_OUT_OF_RANGE',
    });
    assert.throws(() => hkdfSync('unknown', 'a', '', Buffer.alloc(1025), 10), {
      code: 'ERR_OUT_OF_RANGE',
    });

    {
      const p = deferredPromise();
      hkdf('sha512', 'a', '', '', 64 * 255 + 1, (err, asyncResult) => {
        if (err) {
          return p.reject(err);
        }
        p.resolve();
      });
      await assert.rejects(p.promise);
    }
    assert.throws(() => hkdfSync('sha512', 'a', '', '', 64 * 255 + 1), {
      name: 'RangeError',
    });
  },
};

async function hkdfTestAlg([hash, secret, salt, info, length]) {
  {
    const syncResult = hkdfSync(hash, secret, salt, info, length);
    assert.ok(syncResult instanceof ArrayBuffer);
    let is_async = false;
    const p = deferredPromise();

    hkdf(hash, secret, salt, info, length, (err, asyncResult) => {
      if (err) return p.reject(err);
      assert.ok(is_async);
      assert.ok(asyncResult instanceof ArrayBuffer);
      assert.deepStrictEqual(syncResult, asyncResult);
      p.resolve();
    });
    // Keep this after the hkdf call above. This verifies
    // that the callback is invoked asynchronously.
    is_async = true;
    await p.promise;
  }

  {
    const buf_secret = Buffer.from(secret);
    const buf_salt = Buffer.from(salt);
    const buf_info = Buffer.from(info);
    const p = deferredPromise();

    const syncResult = hkdfSync(hash, buf_secret, buf_salt, buf_info, length);
    hkdf(hash, buf_secret, buf_salt, buf_info, length, (err, asyncResult) => {
      if (err) return p.reject(err);
      assert.deepStrictEqual(syncResult, asyncResult);
      p.resolve();
    });
    await p.promise;
  }

  // Disabled for now, requires KeyObject support
  // {
  //   const key_secret = createSecretKey(Buffer.from(secret));
  //   const buf_salt = Buffer.from(salt);
  //   const buf_info = Buffer.from(info);
  //   const p = deferredPromise();
  //
  //   const syncResult = hkdfSync(hash, key_secret, buf_salt, buf_info, length);
  //   hkdf(hash, key_secret, buf_salt, buf_info, length, (err, asyncResult) => {
  //        if (err) return p.reject(err);
  //        assert.deepStrictEqual(syncResult, asyncResult);
  //        p.resolve();
  //   });
  //   await p.promise;
  // }

  {
    const p = deferredPromise();
    const ta_secret = new Uint8Array(Buffer.from(secret));
    const ta_salt = new Uint16Array(Buffer.from(salt));
    const ta_info = new Uint32Array(Buffer.from(info));

    const syncResult = hkdfSync(hash, ta_secret, ta_salt, ta_info, length);
    hkdf(hash, ta_secret, ta_salt, ta_info, length, (err, asyncResult) => {
      if (err) return p.reject(err);
      assert.deepStrictEqual(syncResult, asyncResult);
      p.resolve();
    });
    await p.promise;

    const syncResultBuf = hkdfSync(
      hash,
      ta_secret.buffer,
      ta_salt.buffer,
      ta_info.buffer,
      length
    );
    assert.deepStrictEqual(syncResult, syncResultBuf);
  }

  {
    const p = deferredPromise();
    const ta_secret = new Uint8Array(Buffer.from(secret));
    const a_salt = new ArrayBuffer(0);
    const a_info = new ArrayBuffer(1);

    const syncResult = hkdfSync(hash, ta_secret.buffer, a_salt, a_info, length);
    hkdf(hash, ta_secret, a_salt, a_info, length, (err, asyncResult) => {
      if (err) return p.reject(err);
      assert.deepStrictEqual(syncResult, asyncResult);
      p.resolve();
    });
    await p.promise;
  }
}

export const hkdf_correctness_tests = {
  async test(ctrl, env, ctx) {
    const algorithms = [
      ['sha256', 'secret', 'salt', 'info', 10],
      ['sha256', '', '', '', 10],
      ['sha256', '', 'salt', '', 10],
      ['sha512', 'secret', 'salt', '', 15],
    ];
    for (const element of algorithms) {
      await hkdfTestAlg(element);
    }

    getHashes().forEach((hash) => {
      assert.ok(hkdfSync(hash, 'key', 'salt', 'info', 5));
    });
  },
};
