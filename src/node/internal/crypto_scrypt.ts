// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Copyright Joyent, Inc. and other Node contributors.
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

/* TODO: the following is adopted code, enabling linting one day */
/* eslint-disable */

'use strict';

import { default as cryptoImpl } from 'node-internal:crypto';

import {
  validateFunction,
  validateInteger,
  validateUint32,
} from 'node-internal:validators';

import { Buffer } from 'node-internal:internal_buffer';

type ArrayLike = cryptoImpl.ArrayLike;

import { kMaxLength } from 'node-internal:internal_buffer';

import { getArrayBufferOrView } from 'node-internal:crypto_util';

import { ERR_INVALID_ARG_VALUE } from 'node-internal:internal_errors';

export interface ValidatedScryptOptions {
  password: ArrayLike;
  salt: ArrayLike;
  keylen: number;
  N: number;
  r: number;
  p: number;
  maxmem: number;
}

export interface ScryptOptions {
  N?: number;
  r?: number;
  p?: number;
  maxmem?: number;
  cost?: number;
  blockSize?: number;
  parallelization?: number;
}

const defaults: ScryptOptions = {
  N: 16384,
  r: 8,
  p: 1,
  maxmem: 32 << 20,
};

function validateParameters(
  password: ArrayLike,
  salt: ArrayLike,
  keylen: number,
  options: ScryptOptions
): ValidatedScryptOptions {
  // TODO(soon): Add support for KeyObject input.
  password = getArrayBufferOrView(password, 'password');
  salt = getArrayBufferOrView(salt, 'salt');

  if (password.byteLength >= kMaxLength) {
    throw new ERR_INVALID_ARG_VALUE('password', password, 'is too big');
  }
  if (salt.byteLength >= kMaxLength) {
    throw new ERR_INVALID_ARG_VALUE('salt', salt, 'is too big');
  }

  validateInteger(keylen, 'keylen', 0, kMaxLength);

  let { N, r, p, maxmem } = defaults;
  if (options && options !== defaults) {
    const has_N = options.N !== undefined;
    if (has_N) {
      N = options.N;
      validateUint32(N, 'N');
    }
    if (options.cost !== undefined) {
      if (has_N) throw new ERR_INVALID_ARG_VALUE('cost', options.cost);
      N = options.cost;
      validateUint32(N, 'cost');
    }
    const has_r = options.r !== undefined;
    if (has_r) {
      r = options.r;
      validateUint32(r, 'r');
    }
    if (options.blockSize !== undefined) {
      if (has_r)
        throw new ERR_INVALID_ARG_VALUE('blockSize', options.blockSize);
      r = options.blockSize;
      validateUint32(r, 'blockSize');
    }
    const has_p = options.p !== undefined;
    if (has_p) {
      p = options.p;
      validateUint32(p, 'p');
    }
    if (options.parallelization !== undefined) {
      if (has_p)
        throw new ERR_INVALID_ARG_VALUE(
          'parallelization',
          options.parallelization
        );
      p = options.parallelization;
      validateUint32(p, 'parallelization');
    }
    if (options.maxmem !== undefined) {
      maxmem = options.maxmem;
      validateInteger(maxmem, 'maxmem', 0);
    }
    if (N === 0) N = defaults.N;
    if (r === 0) r = defaults.r;
    if (p === 0) p = defaults.p;
    if (maxmem === 0) maxmem = defaults.maxmem;
  }

  return { password, salt, keylen, N: N!, r: r!, p: p!, maxmem: maxmem! };
}

type Callback = (err: Error | null, derivedKey?: ArrayBuffer) => void;
type OptionsOrCallback = ScryptOptions | Callback;

export function scrypt(
  password: ArrayLike,
  salt: ArrayLike,
  keylen: number,
  options: OptionsOrCallback,
  callback: OptionsOrCallback = defaults
) {
  if (callback === defaults) {
    callback = options;
    options = defaults;
  }

  let N: number;
  let r: number;
  let p: number;
  let maxmem: number;
  ({ password, salt, keylen, N, r, p, maxmem } = validateParameters(
    password,
    salt,
    keylen,
    options as ScryptOptions
  ));

  validateFunction(callback, 'callback');

  new Promise<ArrayBuffer>((res, rej) => {
    try {
      res(cryptoImpl.getScrypt(password, salt, N, r, p, maxmem, keylen));
    } catch (err) {
      rej(err);
    }
  }).then(
    (val: ArrayBuffer) => {
      (callback as Callback)(null, Buffer.from(val));
    },
    (err) => (callback as Callback)(err)
  );
}

export function scryptSync(
  password: ArrayLike,
  salt: ArrayLike,
  keylen: number,
  options: ScryptOptions
): ArrayBuffer {
  let N: number;
  let r: number;
  let p: number;
  let maxmem: number;
  ({ password, salt, keylen, N, r, p, maxmem } = validateParameters(
    password,
    salt,
    keylen,
    options
  ));

  return Buffer.from(
    cryptoImpl.getScrypt(password, salt, N, r, p, maxmem, keylen)
  );
}
