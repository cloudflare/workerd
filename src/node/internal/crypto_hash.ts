// Copyright (c) 2017-2022 Cloudflare, Inc.
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

import { default as cryptoImpl } from 'node-internal:crypto';

import {
  kFinalized,
  kHandle,
  kState,
} from 'node-internal:crypto_util';

import {
  Buffer
} from 'node-internal:internal_buffer';

import {
  ERR_CRYPTO_HASH_FINALIZED,
  ERR_CRYPTO_HASH_UPDATE_FAILED,
  ERR_INVALID_ARG_TYPE,
} from 'node-internal:internal_errors';

import {
  validateEncoding,
  validateString,
  validateUint32,
} from 'node-internal:validators';

import {
  normalizeEncoding
} from 'node-internal:internal_utils';

import {
  isArrayBufferView
} from 'node-internal:internal_types';

import {
  Transform,
  TransformOptions,
  TransformCallback,
} from 'node-internal:streams_transform';

export interface HashOptions extends TransformOptions {
  outputLength?: number;
}

interface _kState {
  [kFinalized]: boolean;
}

interface Hash extends Transform {
  [kHandle]: cryptoImpl.HashHandle;
  [kState]: _kState;
}

// These helper functions are needed because the constructors can
// use new, in which case V8 cannot inline the recursive constructor call
export function createHash(algorithm: string, options?: HashOptions): Hash {
  return new Hash(algorithm, options);
}

let Hash = function(this: Hash, algorithm: string | cryptoImpl.HashHandle,
  options?: HashOptions): Hash {
  if (!(this instanceof Hash))
    return new Hash(algorithm, options);

  const xofLen = typeof options === 'object' ? options.outputLength : undefined;
  if (xofLen !== undefined)
    validateUint32(xofLen, 'options.outputLength');
  if (algorithm instanceof cryptoImpl.HashHandle) {
    this[kHandle] = algorithm.copy(xofLen as number);
  } else {
    validateString(algorithm, 'algorithm');
    this[kHandle] = new cryptoImpl.HashHandle(algorithm, xofLen as number);
  }
  this[kState] = {
    [kFinalized]: false,
  };

  Transform.call(this, options);
  return this;
} as any as { new (algorithm: string | cryptoImpl.HashHandle, options?: HashOptions): Hash; };

Object.setPrototypeOf(Hash.prototype, Transform.prototype);
Object.setPrototypeOf(Hash, Transform);

Hash.prototype.copy = function(this: Hash, options?: HashOptions): Hash {
  const state = this[kState];
  if (state[kFinalized])
    throw new ERR_CRYPTO_HASH_FINALIZED();

  return new Hash(this[kHandle], options);
}

Hash.prototype._transform = function(this: Hash, chunk: Buffer | string | any, encoding: string,
                                     callback: TransformCallback): void {
  if (typeof chunk === 'string') {
    encoding ??= 'utf-8';
    validateEncoding(chunk, encoding);
    encoding = normalizeEncoding(encoding)!;
    chunk = Buffer.from(chunk, encoding);
  }
  this[kHandle].update(chunk);
  callback();
}

Hash.prototype._flush = function(this: Hash, callback: TransformCallback): void {
  this.push(Buffer.from(this[kHandle].digest()));
  callback();
}

Hash.prototype.update = function(this: Hash, data: string | Buffer | ArrayBufferView,
                                 encoding?: string): Hash {
  encoding ??= 'utf8';
  if (encoding === 'buffer') {
    encoding = undefined;
  }

  const state = this[kState];
  if (state[kFinalized])
    throw new ERR_CRYPTO_HASH_FINALIZED();

  if (typeof data === 'string') {
    validateEncoding(data, encoding!);
    encoding = normalizeEncoding(encoding);
    data = Buffer.from(data, encoding);
  } else if (!isArrayBufferView(data)) {
    throw new ERR_INVALID_ARG_TYPE(
      'data', ['string', 'Buffer', 'TypedArray', 'DataView'], data);
  }

  if (!this[kHandle].update(data))
    throw new ERR_CRYPTO_HASH_UPDATE_FAILED();
  return this;
}

Hash.prototype.digest = function(this: Hash, outputEncoding?: string): Buffer | string {
  const state = this[kState];
  if (state[kFinalized])
    throw new ERR_CRYPTO_HASH_FINALIZED();

  // Explicit conversion for backward compatibility.
  const ret = Buffer.from(this[kHandle].digest());
  state[kFinalized] = true;
  if (outputEncoding !== undefined && outputEncoding !== 'buffer') {
    return ret.toString(outputEncoding);
  } else {
    return ret;
  }
}

export {Hash};
