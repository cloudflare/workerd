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

import {
  default as cryptoImpl,
  type SignHandle,
  type VerifyHandle,
  type CreateAsymmetricKeyOptions,
} from 'node-internal:crypto';

import { validateString } from 'node-internal:validators';

import { Buffer } from 'node-internal:internal_buffer';

import {
  KeyObject,
  isKeyObject,
  getKeyObjectHandle,
  createPrivateKey,
  createPublicKey,
} from 'node-internal:crypto_keys';

import {
  ERR_CRYPTO_SIGN_KEY_REQUIRED,
  ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
} from 'node-internal:internal_errors';

import { Writable } from 'node-internal:streams_writable';
import { isArrayBufferView } from 'node-internal:internal_types';

export interface SignOptions {}

export interface Sign extends Writable {
  [kHandle]: SignHandle;
  update(data: any): void;
  sign(privateKey: any): any;
}
export interface Verify extends Writable {
  [kHandle]: VerifyHandle;
  update(data: any): void;
  verify(publicKey: any, signature: any): any;
}

const kHandle = Symbol('kHandle');

// Uses old-style class syntax for Node.js compatibility
export const Sign = function (this: Sign, algorithm: string, options: any) {
  if (!(this instanceof Sign)) return new Sign(algorithm, options);
  validateString(algorithm, 'algorithm');
  this[kHandle] = new cryptoImpl.SignHandle(algorithm);
  Writable.call(this, options);
  return this;
} as any as {
  new (algorithm: string, options?: SignOptions): Sign;
};
Object.setPrototypeOf(Sign.prototype, Writable.prototype);
Object.setPrototypeOf(Sign, Writable);

Sign.prototype._write = function _write(
  chunk: string | ArrayBufferView,
  encoding: string | undefined | null,
  callback: (err?: any) => void
) {
  this.update(chunk, encoding);
  callback();
};

Sign.prototype.update = function (
  this: Sign,
  data: string | ArrayBufferView,
  encoding?: string
) {
  if (typeof data === 'string') {
    data = Buffer.from(data, encoding);
  }
  if (!isArrayBufferView(data)) {
    throw new ERR_INVALID_ARG_TYPE(
      'data',
      ['string', 'Buffer', 'TypedArray', 'DataView'],
      data
    );
  }
  this[kHandle].update(data);
  return this;
};

function getIntOption(name: string, options: any): number | undefined {
  const value = options[name];
  if (value !== undefined) {
    if (value === value >> 0) {
      return value;
    }
    throw new ERR_INVALID_ARG_VALUE(`options.${name}`, value);
  }
  return undefined;
}

function getDSASignatureEncoding(options: any): number {
  if (typeof options === 'object') {
    const { dsaEncoding = 'der' } = options;
    if (dsaEncoding === 'der')
      return 0; // kSigEncDER;
    else if (dsaEncoding === 'ieee-p1363') return 1; // kSigEncP1363
    throw new ERR_INVALID_ARG_VALUE('options.dsaEncoding', dsaEncoding);
  }

  return 0;
}

function getPrivateKey(options: any): CryptoKey {
  if (options instanceof CryptoKey) {
    return options as CryptoKey;
  } else if (isKeyObject(options)) {
    const keyObject = options as KeyObject;
    if (keyObject.type === 'secret') {
      throw new ERR_INVALID_ARG_TYPE(
        'options',
        ['PublicKeyObject', 'PrivateKeyObject', 'CryptoKey', 'object'],
        keyObject.type
      );
    }
    if (keyObject.type === 'public') {
      throw new ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE('public', 'private');
    }
    return getKeyObjectHandle(keyObject);
  } else {
    return getKeyObjectHandle(
      createPrivateKey(options as CreateAsymmetricKeyOptions)
    );
  }
}

Sign.prototype.sign = function (
  this: Sign,
  options: any,
  encoding?: string
): Buffer | string {
  if (!options) {
    throw new ERR_CRYPTO_SIGN_KEY_REQUIRED();
  }

  const key = getPrivateKey(options);

  // Options specific to RSA
  const rsaPadding = getIntOption('padding', options);
  const pssSaltLength = getIntOption('saltLength', options);

  // Options specific to (EC)DSA
  const dsaSigEnc = getDSASignatureEncoding(options);

  const res = Buffer.from(
    this[kHandle].sign(key, rsaPadding, pssSaltLength, dsaSigEnc)
  );

  if (encoding && encoding !== 'buffer') {
    return res.toString(encoding);
  }

  return res;
};

// Uses old-style class syntax for Node.js compatibility
export const Verify = function (this: Verify, algorithm: string, options: any) {
  if (!(this instanceof Verify)) return new Verify(algorithm, options);
  validateString(algorithm, 'algorithm');
  this[kHandle] = new cryptoImpl.VerifyHandle(algorithm);
  Writable.call(this, options);
  return this;
} as any as {
  new (algorithm: string, options?: SignOptions): Verify;
};
Object.setPrototypeOf(Verify.prototype, Writable.prototype);
Object.setPrototypeOf(Verify, Writable);

Verify.prototype._write = function _write(
  chunk: string | ArrayBufferView,
  encoding: string | undefined | null,
  callback: (err?: unknown) => void
) {
  this.update(chunk, encoding);
  callback();
};

Verify.prototype.update = function (
  this: Verify,
  data: string | ArrayBufferView,
  encoding?: string
) {
  if (typeof data === 'string') {
    data = Buffer.from(data, encoding);
  }
  if (!isArrayBufferView(data)) {
    throw new ERR_INVALID_ARG_TYPE(
      'data',
      ['string', 'Buffer', 'TypedArray', 'DataView'],
      data
    );
  }
  this[kHandle].update(data);
  return this;
};

Verify.prototype.verify = function (
  this: Verify,
  options: any,
  signature: ArrayBufferView | string,
  encoding: string = 'utf8'
) {
  if (!options) {
    throw new ERR_CRYPTO_SIGN_KEY_REQUIRED();
  }

  let key: CryptoKey;
  if (options instanceof CryptoKey) {
    key = options as CryptoKey;
  } else if (isKeyObject(options)) {
    key = getKeyObjectHandle(options as KeyObject);
  } else {
    key = getKeyObjectHandle(
      createPublicKey(options as CreateAsymmetricKeyOptions)
    );
  }
  if (!(key instanceof CryptoKey)) {
    throw new ERR_INVALID_ARG_TYPE(
      'options',
      ['KeyObject', 'CryptoKey', 'object'],
      key
    );
  }

  // Options specific to RSA
  const rsaPadding = getIntOption('padding', options);
  const pssSaltLength = getIntOption('saltLength', options);

  // Options specific to (EC)DSA
  const dsaSigEnc = getDSASignatureEncoding(options);

  if (typeof signature === 'string') {
    signature = Buffer.from(signature, encoding);
  }

  return this[kHandle].verify(
    key,
    signature,
    rsaPadding,
    pssSaltLength,
    dsaSigEnc
  );
};

export function createSign(algorithm: string, options: any) {
  return new Sign(algorithm, options);
}

export function createVerify(algorithm: string, options: any) {
  return new Verify(algorithm, options);
}

type SignCallback = (err: any, signature?: Buffer) => void;
type VerifyCallback = (err: any, valid?: boolean) => void;

export function sign(
  algorithm: string | null | undefined,
  data: BufferSource,
  options: any,
  callback?: SignCallback
): Buffer | void {
  if (algorithm != null) {
    validateString(algorithm, 'algorithm');
  } else {
    algorithm = undefined;
  }
  if (!isArrayBufferView(data)) {
    throw new ERR_INVALID_ARG_TYPE(
      'data',
      ['Buffer', 'TypedArray', 'DataView'],
      data
    );
  }

  if (!options) {
    throw new ERR_CRYPTO_SIGN_KEY_REQUIRED();
  }

  const key = getPrivateKey(options);

  // Options specific to RSA
  const rsaPadding = getIntOption('padding', options);
  const pssSaltLength = getIntOption('saltLength', options);

  // Options specific to (EC)DSA
  const dsaSigEnc = getDSASignatureEncoding(options);

  if (callback === undefined) {
    return Buffer.from(
      cryptoImpl.signOneShot(
        key,
        algorithm,
        data,
        rsaPadding,
        pssSaltLength,
        dsaSigEnc
      )
    );
  }

  try {
    const signature = Buffer.from(
      cryptoImpl.signOneShot(
        key,
        algorithm,
        data,
        rsaPadding,
        pssSaltLength,
        dsaSigEnc
      )
    );
    queueMicrotask(() => callback(null, signature));
  } catch (err) {
    queueMicrotask(() => callback(err));
  }
}

export function verify(
  algorithm: string | null | undefined,
  data: BufferSource,
  options: any,
  signature: BufferSource,
  callback?: VerifyCallback
): boolean | undefined {
  // Node.js allows the algorithm to be either undefined or null, in which
  // case we just normalize it to undefined.
  if (algorithm != null) {
    validateString(algorithm, 'algorithm');
  } else {
    algorithm = undefined;
  }

  if (!isArrayBufferView(data)) {
    throw new ERR_INVALID_ARG_TYPE(
      'data',
      ['Buffer', 'TypedArray', 'DataView'],
      data
    );
  }

  if (!isArrayBufferView(signature)) {
    throw new ERR_INVALID_ARG_TYPE(
      'signature',
      ['Buffer', 'TypedArray', 'DataView'],
      signature
    );
  }

  if (!options) {
    throw new ERR_CRYPTO_SIGN_KEY_REQUIRED();
  }

  let key: CryptoKey;
  if (options instanceof CryptoKey) {
    key = options;
  } else if (isKeyObject(options)) {
    key = getKeyObjectHandle(options as KeyObject);
  } else {
    key = getKeyObjectHandle(
      createPublicKey(options as CreateAsymmetricKeyOptions)
    );
  }
  if (!(key instanceof CryptoKey)) {
    throw new ERR_INVALID_ARG_TYPE(
      'options',
      ['KeyObject', 'CryptoKey', 'object'],
      key
    );
  }

  // Options specific to RSA
  const rsaPadding = getIntOption('padding', options);
  const pssSaltLength = getIntOption('saltLength', options);

  // Options specific to (EC)DSA
  const dsaSigEnc = getDSASignatureEncoding(options);

  if (callback === undefined) {
    return cryptoImpl.verifyOneShot(
      key,
      algorithm,
      data,
      signature,
      rsaPadding,
      pssSaltLength,
      dsaSigEnc
    );
  }

  try {
    callback(
      null,
      cryptoImpl.verifyOneShot(
        key,
        algorithm,
        data,
        signature,
        rsaPadding,
        pssSaltLength,
        dsaSigEnc
      )
    );
  } catch (err) {
    queueMicrotask(() => callback(err));
  }
  return; // explicit return is necessary to squelch typescript warning.
}
