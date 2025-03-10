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

import { Buffer } from 'node-internal:internal_buffer';

import { default as cryptoImpl, type ECDHFormat } from 'node-internal:crypto';
type ArrayLike = cryptoImpl.ArrayLike;

import {
  isKeyObject,
  getKeyObjectHandle,
  type PrivateKeyObject,
  type PublicKeyObject,
} from 'node-internal:crypto_keys';

import {
  ERR_CRYPTO_ECDH_INVALID_PUBLIC_KEY,
  ERR_INVALID_ARG_TYPE,
} from 'node-internal:internal_errors';

import {
  validateInt32,
  validateObject,
  validateOneOf,
  validateString,
} from 'node-internal:validators';

import {
  isArrayBufferView,
  isAnyArrayBuffer,
} from 'node-internal:internal_types';

import {
  getArrayBufferOrView,
  toBuf,
  kHandle,
} from 'node-internal:crypto_util';

const DH_GENERATOR = 2;

declare class SharedDiffieHellman {
  public generateKeys: typeof dhGenerateKeys;
  public computeSecret: typeof dhComputeSecret;
  public getPrime: typeof dhGetPrime;
  public getGenerator: typeof dhGetGenerator;
  public getPublicKey: typeof dhGetPublicKey;
  public getPrivateKey: typeof dhGetPrivateKey;
  public setPrivateKey: typeof dhSetPrivateKey;
  public setPublicKey: typeof dhSetPublicKey;
}

declare class DiffieHellman extends SharedDiffieHellman {
  public [kHandle]: cryptoImpl.DiffieHellmanHandle;

  public constructor(
    sizeOrKey: number | ArrayLike,
    keyEncoding?: number | string,
    generator?: number | ArrayLike,
    genEncoding?: string
  );
}

function DiffieHellman(
  this: unknown,
  sizeOrKey: number | ArrayLike,
  keyEncoding?: number | string,
  generator?: number | ArrayLike,
  genEncoding?: string
): DiffieHellman {
  if (!(this instanceof DiffieHellman)) {
    return new DiffieHellman(sizeOrKey, keyEncoding, generator, genEncoding);
  }
  if (
    typeof sizeOrKey !== 'number' &&
    typeof sizeOrKey !== 'string' &&
    !isArrayBufferView(sizeOrKey) &&
    !isAnyArrayBuffer(sizeOrKey)
  ) {
    throw new ERR_INVALID_ARG_TYPE(
      'sizeOrKey',
      ['number', 'string', 'ArrayBuffer', 'Buffer', 'TypedArray', 'DataView'],
      sizeOrKey
    );
  }

  // Sizes < 0 don't make sense but they _are_ accepted (and subsequently
  // rejected with ERR_OSSL_BN_BITS_TOO_SMALL) by OpenSSL. The glue code
  // in node_crypto.cc accepts values that are IsInt32() for that reason
  // and that's why we do that here too.
  if (typeof sizeOrKey === 'number') validateInt32(sizeOrKey, 'sizeOrKey');

  if (
    keyEncoding &&
    keyEncoding !== 'buffer' &&
    !Buffer.isEncoding(keyEncoding)
  ) {
    genEncoding = generator as string;
    generator = keyEncoding;
    keyEncoding = 'utf-8'; // default encoding
  }

  keyEncoding ??= 'utf-8';
  genEncoding ??= 'utf-8';

  if (typeof sizeOrKey !== 'number')
    sizeOrKey = toBuf(sizeOrKey, keyEncoding as string);

  if (!generator) {
    generator = DH_GENERATOR;
  } else if (typeof generator === 'number') {
    validateInt32(generator, 'generator');
  } else if (typeof generator === 'string') {
    generator = toBuf(generator, genEncoding);
  } else if (!isArrayBufferView(generator) && !isAnyArrayBuffer(generator)) {
    throw new ERR_INVALID_ARG_TYPE(
      'generator',
      ['number', 'string', 'ArrayBuffer', 'Buffer', 'TypedArray', 'DataView'],
      generator
    );
  }

  this[kHandle] = new cryptoImpl.DiffieHellmanHandle(sizeOrKey, generator);
  Object.defineProperty(DiffieHellman.prototype, 'verifyError', {
    get: function (this: DiffieHellman) {
      return this[kHandle].getVerifyError();
    },
    configurable: true,
    enumerable: true,
  });
  return this;
}

declare class DiffieHellmanGroup extends SharedDiffieHellman {
  public [kHandle]: cryptoImpl.DiffieHellmanHandle;
  public constructor(name: string);
}

function DiffieHellmanGroup(this: unknown, name: string): DiffieHellmanGroup {
  if (!(this instanceof DiffieHellmanGroup)) {
    return new DiffieHellmanGroup(name);
  }

  // The C++-based handle is shared between both classes, so DiffieHellmanGroupHandle() is merely
  // a different constructor for a DiffieHellmanHandle.
  this[kHandle] = cryptoImpl.DiffieHellmanGroupHandle(name);
  Object.defineProperty(DiffieHellmanGroup.prototype, 'verifyError', {
    get: function (this: DiffieHellmanGroup): number {
      return this[kHandle].getVerifyError();
    },
    configurable: true,
    enumerable: true,
  });
  return this;
}

DiffieHellmanGroup.prototype.generateKeys =
  DiffieHellman.prototype.generateKeys = dhGenerateKeys;
DiffieHellmanGroup.prototype.computeSecret =
  DiffieHellman.prototype.computeSecret = dhComputeSecret;
DiffieHellmanGroup.prototype.getPrime = DiffieHellman.prototype.getPrime =
  dhGetPrime;
DiffieHellmanGroup.prototype.getGenerator =
  DiffieHellman.prototype.getGenerator = dhGetGenerator;
DiffieHellmanGroup.prototype.getPublicKey =
  DiffieHellman.prototype.getPublicKey = dhGetPublicKey;
DiffieHellmanGroup.prototype.getPrivateKey =
  DiffieHellman.prototype.getPrivateKey = dhGetPrivateKey;
DiffieHellman.prototype.setPublicKey = dhSetPublicKey;
DiffieHellman.prototype.setPrivateKey = dhSetPrivateKey;

export { DiffieHellman, DiffieHellmanGroup };

type DHLike = DiffieHellman | DiffieHellmanGroup;
function dhGenerateKeys(this: DHLike, encoding?: string): Buffer | string {
  const keys = this[kHandle].generateKeys();
  return encode(keys, encoding);
}

function dhComputeSecret(
  this: DHLike,
  key: ArrayLike,
  inEnc?: string,
  outEnc?: string
): Buffer | string {
  key = getArrayBufferOrView(key, 'key', inEnc);
  const ret = this[kHandle].computeSecret(key);
  if (typeof ret === 'string') throw new ERR_CRYPTO_ECDH_INVALID_PUBLIC_KEY();
  return encode(ret, outEnc);
}

function dhGetPrime(this: DHLike, encoding?: string): Buffer | string {
  const prime = this[kHandle].getPrime();
  return encode(prime, encoding);
}

function dhGetGenerator(this: DHLike, encoding?: string): Buffer | string {
  const generator = this[kHandle].getGenerator();
  return encode(generator, encoding);
}

function dhGetPublicKey(this: DHLike, encoding?: string): Buffer | string {
  const key = this[kHandle].getPublicKey();
  return encode(key, encoding);
}

function dhGetPrivateKey(this: DHLike, encoding?: string): Buffer | string {
  const key = this[kHandle].getPrivateKey();
  return encode(key, encoding);
}

function dhSetPublicKey(
  this: DiffieHellman,
  key: ArrayLike,
  encoding?: string
): DiffieHellman {
  key = getArrayBufferOrView(key, 'key', encoding);
  this[kHandle].setPublicKey(key);
  return this;
}

function dhSetPrivateKey(
  this: DiffieHellman,
  key: ArrayLike,
  encoding?: string
): DiffieHellman {
  key = getArrayBufferOrView(key, 'key', encoding);
  this[kHandle].setPrivateKey(key);
  return this;
}

function encode(buffer: ArrayBuffer, encoding?: string): Buffer | string {
  if (encoding && encoding !== 'buffer')
    return Buffer.from(buffer).toString(encoding);
  return Buffer.from(buffer);
}

export function createDiffieHellman(
  sizeOrKey: number | ArrayLike,
  keyEncoding?: number | string,
  generator?: number | ArrayLike,
  genEncoding?: string
): DiffieHellman {
  return new DiffieHellman(sizeOrKey, keyEncoding, generator, genEncoding);
}

export function createDiffieHellmanGroup(name: string): DiffieHellmanGroup {
  return new DiffieHellmanGroup(name);
}

export function getDiffieHellman(name: string): DiffieHellmanGroup {
  return createDiffieHellmanGroup(name);
}

export interface DiffieHellmanKeyPair {
  publicKey: PublicKeyObject;
  privateKey: PrivateKeyObject;
}

export function diffieHellman(options: DiffieHellmanKeyPair): Buffer {
  validateObject(options, 'options');
  const { publicKey, privateKey } = options;
  if (!isKeyObject(publicKey)) {
    throw new ERR_INVALID_ARG_TYPE('options.publicKey', 'KeyObject', publicKey);
  }
  if (!isKeyObject(privateKey)) {
    throw new ERR_INVALID_ARG_TYPE(
      'options.privateKey',
      'KeyObject',
      privateKey
    );
  }
  if (publicKey.type !== 'public') {
    throw new ERR_INVALID_ARG_TYPE(
      'options.publicKey',
      'public key',
      publicKey
    );
  }
  if (privateKey.type !== 'private') {
    throw new ERR_INVALID_ARG_TYPE(
      'options.privateKey',
      'private key',
      privateKey
    );
  }
  if (
    publicKey.asymmetricKeyType !== 'dh' ||
    privateKey.asymmetricKeyType !== 'dh'
  ) {
    throw new ERR_INVALID_ARG_TYPE('options', 'DiffieHellman keys', options);
  }
  const res = cryptoImpl.statelessDH(
    getKeyObjectHandle(privateKey),
    getKeyObjectHandle(publicKey)
  );
  return Buffer.from(res);
}

// =============================================================================

export interface ECDH {
  [kHandle]: cryptoImpl.ECDHHandle;
  computeSecret(
    otherPublicKey: string | ArrayBufferView | ArrayBuffer,
    inputEncoding?: string,
    outputEncoding?: string
  ): Buffer | string;
  generateKeys(encoding?: string, format?: string): Buffer | string;
  getPrivateKey(encoding?: string): Buffer | string;
  getPublicKey(encoding?: string, format?: string): Buffer | string;
  setPrivateKey(
    key: string | ArrayBufferView | ArrayBuffer,
    encoding?: string
  ): void;
}

export const ECDH = function (this: ECDH, curveName: string) {
  if (!(this instanceof ECDH)) {
    return new ECDH(curveName);
  }
  validateString(curveName, 'curveName');
  this[kHandle] = new cryptoImpl.ECDHHandle(curveName);
  return this;
} as unknown as {
  new (curveName: string): ECDH;
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
ECDH.prototype.computeSecret = function (
  this: ECDH,
  otherPublicKey: string | ArrayBufferView | ArrayBuffer,
  inputEncoding?: string,
  outputEncoding?: string
): Buffer | string {
  if (typeof otherPublicKey === 'string') {
    otherPublicKey = Buffer.from(otherPublicKey, inputEncoding);
  }
  if (!isArrayBufferView(otherPublicKey)) {
    throw new ERR_INVALID_ARG_TYPE(
      'otherPublicKey',
      ['string', 'Buffer', 'TypedArray', 'DataView'],
      otherPublicKey
    );
  }
  if (inputEncoding != null) {
    validateString(inputEncoding, 'inputEncoding');
  }
  if (outputEncoding != null) {
    validateString(outputEncoding, 'outputEncoding');
  }
  const ret = this[kHandle].computeSecret(otherPublicKey);
  if (typeof outputEncoding === 'string') {
    return Buffer.from(ret).toString(outputEncoding);
  }
  return Buffer.from(ret);
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
ECDH.prototype.generateKeys = function (
  this: ECDH,
  encoding?: string,
  format?: string
): Buffer | string {
  if (encoding != null) {
    validateString(encoding, 'encoding');
  }
  if (format != null) {
    validateOneOf(format, 'format', ['compressed', 'uncompressed', 'hybrid']);
  }
  this[kHandle].generateKeys();
  return this.getPublicKey(encoding, format);
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
ECDH.prototype.getPrivateKey = function (
  this: ECDH,
  encoding?: string
): Buffer | string {
  if (encoding != null) {
    validateString(encoding, 'encoding');
  }
  const pvt = this[kHandle].getPrivateKey();
  if (typeof encoding === 'string') {
    return Buffer.from(pvt).toString(encoding);
  }
  return Buffer.from(pvt);
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
ECDH.prototype.getPublicKey = function (
  this: ECDH,
  encoding?: string,
  format: ECDHFormat = 'uncompressed'
): Buffer | string {
  if (encoding != null) {
    validateString(encoding, 'encoding');
  }
  validateOneOf(format, 'format', ['compressed', 'uncompressed', 'hybrid']);
  const pub = this[kHandle].getPublicKey(format);
  if (typeof encoding === 'string') {
    return Buffer.from(pub).toString(encoding);
  }
  return Buffer.from(pub);
};

// eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
ECDH.prototype.setPrivateKey = function (
  this: ECDH,
  key: string | ArrayBufferView | ArrayBuffer,
  encoding?: string
): void {
  if (encoding != null) {
    validateString(encoding, 'encoding');
  }
  if (typeof key === 'string') {
    key = Buffer.from(key, encoding);
  }
  if (!isArrayBufferView(key)) {
    throw new ERR_INVALID_ARG_TYPE(
      'key',
      ['string', 'Buffer', 'TypedArray', 'DataView'],
      key
    );
  }
  this[kHandle].setPrivateKey(key);
};

// eslint-disable-next-line @typescript-eslint/no-explicit-any,@typescript-eslint/no-unsafe-member-access
(ECDH as any).convertKey = function (
  key: string | ArrayBufferView | ArrayBuffer,
  curve: string,
  inputEncoding?: string,
  outputEncoding?: string,
  format: ECDHFormat = 'uncompressed'
): Buffer | string {
  if (typeof key === 'string') {
    key = Buffer.from(key, inputEncoding);
  }
  if (!isArrayBufferView(key)) {
    throw new ERR_INVALID_ARG_TYPE(
      'key',
      ['string', 'Buffer', 'TypedArray', 'DataView'],
      key
    );
  }
  validateString(curve, 'curve');
  if (inputEncoding != null) {
    validateString(inputEncoding, 'inputEncoding');
  }
  if (outputEncoding != null) {
    validateString(outputEncoding, 'outputEncoding');
  }
  validateOneOf(format, 'format', ['compressed', 'uncompressed', 'hybrid']);
  const ret = cryptoImpl.ECDHHandle.convertKey(key, curve, format);
  if (typeof outputEncoding === 'string') {
    return Buffer.from(ret).toString(outputEncoding);
  }
  return Buffer.from(ret);
};

export function createECDH(curveName: string): ECDH {
  return new ECDH(curveName);
}
