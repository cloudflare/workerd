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

/* todo: the following is adopted code, enabling linting one day */
/* eslint-disable */

'use strict';

import { Buffer } from 'node-internal:internal_buffer';

import {
  isAnyArrayBuffer,
  isArrayBufferView,
} from 'node-internal:internal_types';

import { ERR_INVALID_ARG_TYPE } from 'node-internal:internal_errors';

import { validateString } from 'node-internal:validators';

import { default as cryptoImpl } from 'node-internal:crypto';
type ArrayLike = cryptoImpl.ArrayLike;

export const kHandle = Symbol('kHandle');
export const kFinalized = Symbol('kFinalized');
export const kState = Symbol('kFinalized');

export function getStringOption(options: any, key: string) {
  let value;
  if (options && (value = options[key]) != null)
    validateString(value, `options.${key}`);
  return value;
}

export function getArrayBufferOrView(
  buffer: Buffer | ArrayBuffer | ArrayBufferView | string,
  name: string,
  encoding?: string
): Buffer | ArrayBuffer | ArrayBufferView {
  if (isAnyArrayBuffer(buffer)) return buffer as ArrayBuffer;
  if (typeof buffer === 'string') {
    if (encoding === undefined || encoding === 'buffer') {
      encoding = 'utf8';
    }
    return Buffer.from(buffer, encoding);
  }
  if (!isArrayBufferView(buffer)) {
    throw new ERR_INVALID_ARG_TYPE(
      name,
      ['string', 'ArrayBuffer', 'Buffer', 'TypedArray', 'DataView'],
      buffer
    );
  }
  return buffer;
}

/**
 * 48 is the ASCII code for '0', 97 is the ASCII code for 'a'.
 * @param {number} number An integer between 0 and 15.
 * @returns {number} corresponding to the ASCII code of the hex representation
 *                   of the parameter.
 */
export const numberToHexCharCode = (number: number): number =>
  (number < 10 ? 48 : 87) + number;

/**
 * @param {ArrayBuffer} buf An ArrayBuffer.
 * @return {bigint}
 */
export function arrayBufferToUnsignedBigInt(buf: ArrayBuffer): bigint {
  const length = buf.byteLength;
  const chars = Array<number>(length * 2);
  const view = new DataView(buf);

  for (let i = 0; i < length; i++) {
    const val = view.getUint8(i);
    chars[2 * i] = numberToHexCharCode(val >> 4);
    chars[2 * i + 1] = numberToHexCharCode(val & 0xf);
  }

  return BigInt(`0x${String.fromCharCode.apply(null, chars)}`);
}

// This is here because many functions accepted binary strings without
// any explicit encoding in older versions of node, and we don't want
// to break them unnecessarily.
export function toBuf(
  val: ArrayLike,
  encoding?: string
): Buffer | ArrayBuffer | ArrayBufferView {
  if (typeof val === 'string') {
    if (encoding === 'buffer') {
      encoding = 'utf8';
    }
    return Buffer.from(val, encoding);
  }
  return val;
}

export function validateByteSource(
  val: ArrayLike,
  name: string
): Buffer | ArrayBuffer | ArrayBufferView {
  val = toBuf(val);

  if (isAnyArrayBuffer(val) || isArrayBufferView(val)) {
    return val;
  }

  throw new ERR_INVALID_ARG_TYPE(
    name,
    ['string', 'ArrayBuffer', 'TypedArray', 'DataView', 'Buffer'],
    val
  );
}
