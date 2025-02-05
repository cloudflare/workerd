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
  validateString,
} from 'node-internal:validators';

import { KeyObject } from 'node-internal:crypto_keys';

type ArrayLike = cryptoImpl.ArrayLike;

import { kMaxLength } from 'node-internal:internal_buffer';

import { toBuf, validateByteSource } from 'node-internal:crypto_util';

import {
  isAnyArrayBuffer,
  isArrayBufferView,
} from 'node-internal:internal_types';

import {
  NodeError,
  ERR_INVALID_ARG_TYPE,
  ERR_OUT_OF_RANGE,
} from 'node-internal:internal_errors';

function validateParameters(
  hash: string,
  key: ArrayLike | KeyObject,
  salt: ArrayLike,
  info: ArrayLike,
  length: number
) {
  // TODO(soon): Add support for KeyObject input.
  if (key instanceof KeyObject) {
    throw new NodeError(
      'ERR_METHOD_NOT_IMPLEMENTED',
      'KeyObject support for hkdf() and ' +
        'hkdfSync() is not yet implemented. Use ArrayBuffer, TypedArray, ' +
        'DataView, or Buffer instead.'
    );
  }

  validateString(hash, 'digest');
  key = prepareKey(key as unknown as ArrayLike);
  salt = validateByteSource(salt, 'salt');
  info = validateByteSource(info, 'info');

  validateInteger(length, 'length', 0, kMaxLength);

  if (info.byteLength > 1024) {
    throw new ERR_OUT_OF_RANGE(
      'info',
      'must not contain more than 1024 bytes',
      info.byteLength
    );
  }

  return {
    hash,
    key,
    salt,
    info,
    length,
  };
}

function prepareKey(key: ArrayLike): ArrayLike {
  key = toBuf(key);

  if (!isAnyArrayBuffer(key) && !isArrayBufferView(key)) {
    throw new ERR_INVALID_ARG_TYPE(
      'ikm',
      [
        'string',
        'SecretKeyObject',
        'ArrayBuffer',
        'TypedArray',
        'DataView',
        'Buffer',
      ],
      key
    );
  }

  return key;
}

export function hkdf(
  hash: string,
  key: ArrayLike | KeyObject,
  salt: ArrayLike,
  info: ArrayLike,
  length: number,
  callback: (err: Error | null, derivedKey?: ArrayBuffer) => void
): void {
  ({ hash, key, salt, info, length } = validateParameters(
    hash,
    key,
    salt,
    info,
    length
  ));

  validateFunction(callback, 'callback');

  new Promise<ArrayBuffer>((res, rej) => {
    try {
      res(cryptoImpl.getHkdf(hash, key as ArrayLike, salt, info, length));
    } catch (err) {
      rej(err);
    }
  }).then(
    (val: ArrayBuffer) => callback(null, val),
    (err) => callback(err)
  );
}

export function hkdfSync(
  hash: string,
  key: ArrayLike | KeyObject,
  salt: ArrayLike,
  info: ArrayLike,
  length: number
): ArrayBuffer {
  ({ hash, key, salt, info, length } = validateParameters(
    hash,
    key,
    salt,
    info,
    length
  ));

  return cryptoImpl.getHkdf(hash, key, salt, info, length);
}
