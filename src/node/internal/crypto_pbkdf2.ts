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
type ArrayLike = cryptoImpl.ArrayLike;
export { ArrayLike };

import { Buffer } from 'node-internal:internal_buffer';

import {
  validateInt32,
  validateFunction,
  validateString,
} from 'node-internal:validators';

import { getArrayBufferOrView } from 'node-internal:crypto_util';

export function pbkdf2Sync(
  password: ArrayLike,
  salt: ArrayLike,
  iterations: number,
  keylen: number,
  digest: string
): Buffer {
  ({ password, salt, iterations, keylen, digest } = check(
    password,
    salt,
    iterations,
    keylen,
    digest
  ));

  const result = cryptoImpl.getPbkdf(
    password,
    salt,
    iterations,
    keylen,
    digest
  );
  return Buffer.from(result);
}

export type Pbkdf2Callback = (err?: Error | null, result?: Buffer) => void;
export function pbkdf2(
  password: ArrayLike,
  salt: ArrayLike,
  iterations: number,
  keylen: number,
  digest: string,
  callback: Pbkdf2Callback
): void {
  if (typeof digest === 'function') {
    // Appease node test cases
    validateString(undefined, 'digest');
  }
  validateFunction(callback, 'callback');
  ({ password, salt, iterations, keylen, digest } = check(
    password,
    salt,
    iterations,
    keylen,
    digest
  ));

  new Promise<ArrayBuffer>((res, rej) => {
    try {
      res(cryptoImpl.getPbkdf(password, salt, iterations, keylen, digest));
    } catch (err) {
      rej(err);
    }
  }).then(
    (val) => callback(null, Buffer.from(val)),
    (err) => callback(err)
  );
}

function check(
  password: ArrayLike | ArrayBufferView,
  salt: ArrayLike | ArrayBufferView,
  iterations: number,
  keylen: number,
  digest: string
): any {
  validateString(digest, 'digest');

  password = getArrayBufferOrView(password, 'password');
  salt = getArrayBufferOrView(salt, 'salt');
  // OpenSSL uses a signed int to represent these values, so we are restricted
  // to the 31-bit range here (which is plenty).
  validateInt32(iterations, 'iterations', 1);
  validateInt32(keylen, 'keylen', 0);

  return { password, salt, iterations, keylen, digest };
}
