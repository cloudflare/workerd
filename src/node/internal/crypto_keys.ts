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
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT ORs
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

/* todo: the following is adopted code, enabling linting one day */
/* eslint-disable */

import { Buffer } from 'node-internal:internal_buffer';

import {
  CryptoKey,
  SecretKeyObjectHandle,
  AsymmetricKeyObjectHandle,
  KeyData,
  KeyObjectType,
  KeyExportResult,
  SecretKeyType,
  ExportOptions,
  AsymmetricKeyDetails,
  AsymmetricKeyType,
  CreateAsymmetricKeyOptions,
  GenerateKeyOptions,
  GenerateKeyPairOptions,
  InnerExportOptions,
  JsonWebKey,
  InnerCreateAsymmetricKeyOptions,
} from 'node-internal:crypto';

import {
  arrayBufferToUnsignedBigInt,
} from 'node-internal:crypto_util';

import {
  isAnyArrayBuffer,
  isArrayBuffer,
  isArrayBufferView,
  isUint8Array,
  isSharedArrayBuffer,
} from 'node-internal:internal_types';

import {
  ERR_INVALID_ARG_TYPE,
  ERR_METHOD_NOT_IMPLEMENTED,
  ERR_INVALID_ARG_VALUE,
} from 'node-internal:internal_errors';

import {
  validateObject,
  validateString,
} from 'node-internal:validators';

const kSkipThrow = Symbol('kSkipThrow');
const kKeyObjectTag = Symbol('kKeyObjectTag');
const kSecretKeyTag = Symbol('kSecretKeyTag');
const kAsymmetricKeyTag = Symbol('kAsymmetricKeyTag');
const kPublicKeyTag = Symbol('kPublicKeyTag');
const kPrivateKeyTag = Symbol('kPrivateKeyTag');
export const kHandle = Symbol('handle');

// In Node.js, the definition of KeyObject is a bit complicated because
// KeyObject instances in Node.js can be transferred via postMessage() and
// structuredClone(), etc, allowing instances to be shared across multiple
// worker threads. We do not implement that model so we're essentially
// re-implementing the Node.js API here instead of just taking their code.

export abstract class KeyObject {
  [kKeyObjectTag] = kKeyObjectTag;
  static from(key: CryptoKey) : KeyObject {
    if (!(key instanceof CryptoKey)) {
      throw new ERR_INVALID_ARG_TYPE('key', 'CryptoKey', key);
    }
    switch (key.type) {
      case 'secret':
        return new SecretKeyObject(kSkipThrow, SecretKeyObjectHandle.fromCryptoKey(key));
      case 'private':
        return new PrivateKeyObject(kSkipThrow, AsymmetricKeyObjectHandle.fromCryptoKey(key));
      case 'public':
        return new PublicKeyObject(kSkipThrow, AsymmetricKeyObjectHandle.fromCryptoKey(key));
    }
  }

  abstract export(options: ExportOptions) : KeyExportResult;
  abstract equals(otherKeyObject: KeyObject) : boolean;
  abstract get type() : KeyObjectType;
  abstract get [kHandle]() : SecretKeyObjectHandle | AsymmetricKeyObjectHandle;

  get [Symbol.toStringTag]() {
    return "KeyObject"
  }
}

abstract class AsymmetricKeyObject extends KeyObject {
  [kAsymmetricKeyTag] = kAsymmetricKeyTag;
  #handle: AsymmetricKeyObjectHandle;
  #details: AsymmetricKeyDetails | undefined = undefined;

  constructor(skipThrow?: typeof kSkipThrow, handle?: AsymmetricKeyObjectHandle) {
    if (skipThrow !== kSkipThrow) {
      // KeyObjects cannot be created with new ... use one of the
      // create or generate methods, or use from to get from a
      // CryptoKey.
      throw new Error('Illegal constructor');
    }
    super();
    this.#handle = handle!;
  }

  get [kHandle]() : AsymmetricKeyObjectHandle {
    return this.#handle;
  }

  get asymmetricKeyDetails() : AsymmetricKeyDetails {
    this.#details ??= (() => {
      const detail = this.#handle.getAsymmetricKeyDetail();
      if (isArrayBuffer(detail.publicExponent)) {
        detail.publicExponent = arrayBufferToUnsignedBigInt(detail.publicExponent as any);
      }
      return detail;
    })();
    return this.#details;
  };

  get asymmetricKeyType() : AsymmetricKeyType {
    return this.#handle.getAsymmetricKeyType();
  }

  export(options: ExportOptions = {}) : KeyExportResult {
    validateObject(options, 'options', {});

    // Yes, converting to any is a bit of a cheat, but it allows us to check
    // each option individually without having to do a bunch of type guards.
    const opts = options as any;
    if (opts.format !== undefined) validateString(opts.format, 'options.format');
    if (opts.type !== undefined) validateString(opts.type, 'options.type');
    if (this.type === 'private') {
      if (opts.cipher !== undefined) {
        validateString(opts.cipher, 'options.cipher');
        if (typeof opts.passphrase === 'string') {
          opts.passphrase = Buffer.from(opts.passphrase, opts.encoding);
        }
        if (!isUint8Array(opts.passphrase)) {
          throw new ERR_INVALID_ARG_TYPE('options.passphrase', [
            'string', 'Uint8Array'
          ], opts.passphrase);
        }
      }
    }

    const ret = this.#handle.export(options as InnerExportOptions);
    if (typeof ret === 'string') return ret;
    if (isUint8Array(ret)) {
      return Buffer.from((ret as Uint8Array).buffer,
                         ret.byteOffset, ret.byteLength) as KeyExportResult;
    } else if (isArrayBuffer(ret)) {
      return Buffer.from(ret as ArrayBuffer, 0, (ret as ArrayBuffer).byteLength);
    }
    return ret;
  }

  equals(otherKeyObject: AsymmetricKeyObject) : boolean {
    if (otherKeyObject[kAsymmetricKeyTag] !== kAsymmetricKeyTag) return false;
    if (this === otherKeyObject ||
        this.#handle === otherKeyObject.#handle) return true;
    if (this.type !== otherKeyObject.type) return false;
    return this.#handle.equals(otherKeyObject.#handle);
  }
}

export class PublicKeyObject extends AsymmetricKeyObject {
  [kPublicKeyTag] = kPublicKeyTag;
  get type() : KeyObjectType { return 'public'; }
}

export class PrivateKeyObject extends AsymmetricKeyObject {
  [kPrivateKeyTag] = kPrivateKeyTag
  get type() : KeyObjectType { return 'private'; }
}

export class SecretKeyObject extends KeyObject {
  [kSecretKeyTag] = kSecretKeyTag;
  #handle: SecretKeyObjectHandle;
  #length: number;

  constructor(skipThrow?: typeof kSkipThrow,
              handle?: SecretKeyObjectHandle,
              length?: number) {
    if (skipThrow !== kSkipThrow) {
      // KeyObjects cannot be created with new ... use one of the
      // create or generate methods, or use from to get from a
      // CryptoKey.
      throw new Error('Illegal constructor');
    }
    super();
    this.#handle = handle!;
    this.#length = length!;
  }

  get [kHandle]() : SecretKeyObjectHandle {
    return this.#handle;
  }

  get symmetricKeySize() {
    return this.#length;
  }

  export(options: ExportOptions = {}) : KeyExportResult {
    validateObject(options, 'options', {});

    // Yes, converting to any is a bit of a cheat, but it allows us to check
    // each option individually without having to do a bunch of type guards.
    const opts = options as any;
    if (opts.format !== undefined) validateString(opts.format, 'options.format');
    if (opts.type !== undefined) validateString(opts.type, 'options.type');

    const ret = this.#handle.export(options as InnerExportOptions);
    if (typeof ret === 'string') return ret;
    if (isUint8Array(ret)) {
      return Buffer.from((ret as Uint8Array).buffer,
                         ret.byteOffset, ret.byteLength) as KeyExportResult;
    } else if (isArrayBuffer(ret)) {
      return Buffer.from(ret as ArrayBuffer, 0, (ret as ArrayBuffer).byteLength);
    }
    return ret;
  }

  equals(otherKeyObject: SecretKeyObject) : boolean {
    if (otherKeyObject[kSecretKeyTag] !== kSecretKeyTag) return false;
    if (this === otherKeyObject ||
        this.#handle === otherKeyObject.#handle) return true;
    if (this.type !== otherKeyObject.type) return false;
    return this.#handle.equals(otherKeyObject.#handle);
  }

  get type() : KeyObjectType { return 'secret'; }
}

type ValidateKeyDataOptions = {
  allowObject?: boolean;
};
function validateKeyData(key: unknown, name: string, options : ValidateKeyDataOptions = {
  allowObject: false,
}) {
  if (key == null ||
    (typeof key !== 'string' &&
     (options.allowObject && typeof key !== 'object') &&
     !isArrayBufferView(key) &&
     !isAnyArrayBuffer(key))) {
    const expected = [
      'string',
      'ArrayBuffer',
      'TypedArray',
      'DataView'
    ];
    if (options.allowObject) expected.push('object');
    throw new ERR_INVALID_ARG_TYPE(name, expected, key);
  }
}

export function createSecretKey(key: string, encoding?: string) : SecretKeyObject;
export function createSecretKey(key: ArrayBuffer | ArrayBufferView) : SecretKeyObject;
export function createSecretKey(key: KeyData, encoding?: string) : SecretKeyObject {
  validateKeyData(key, 'key');
  if (typeof key === 'string') key = Buffer.from(key as string, encoding);
  return new SecretKeyObject(kSkipThrow,
                             new SecretKeyObjectHandle(key),
                             key.byteLength);
}

export function isKeyObject(key: any) : boolean {
  return key != null && key[kKeyObjectTag] === kKeyObjectTag;
}

export function isSecretKeyObject(key: any) : boolean {
  return key != null && key[kSecretKeyTag] === kSecretKeyTag;
}

export function isAsymmetricKeyObject(key: any) : boolean {
  return key != null && key[kAsymmetricKeyTag] === kAsymmetricKeyTag;
}

export function isPrivateKeyObject(key: any) : boolean {
  return key != null && key[kPrivateKeyTag] === kPrivateKeyTag;
}

export function isPublicKeyObject(key: any) : boolean {
  return key != null && key[kPublicKeyTag] === kPublicKeyTag;
}

type KeyDataToValidate = CreateAsymmetricKeyOptions | KeyData | CryptoKey | PrivateKeyObject;

function validateAsymmetricKeyOptions(
    key: KeyDataToValidate,
    type = kPrivateKeyTag) {
  validateKeyData(key, 'key', { allowObject: true });
  let inner : InnerCreateAsymmetricKeyOptions = {};
  inner.isPublicKey = type === kPublicKeyTag;
  inner.format = 'pem';
  if (typeof key === 'string') {
    inner.key = Buffer.from(key as string);
  } else if (isArrayBufferView(key)) {
    inner.key = key as ArrayBufferView;
  } else if (isArrayBuffer(key)) {
    inner.key = key as ArrayBuffer;
  } else if (isSharedArrayBuffer(key)) {
    inner.key = key as SharedArrayBuffer;
  } else if (type === kPublicKeyTag && isKeyObject(key)) {
    // Covers deriving public key from a private key.
    if (!isPrivateKeyObject(key)) {
      throw new ERR_INVALID_ARG_VALUE('key', key, 'must be a private key');
    }
    inner.key = (key as PrivateKeyObject)[kHandle];
  } else if (type === kPublicKeyTag && key instanceof CryptoKey) {
    // Covers deriving public key from a private key.
    if ((key as CryptoKey).type !== 'private') {
      throw new ERR_INVALID_ARG_VALUE('key', key, 'must be a private key');
    }
    inner.key = key as CryptoKey;
  } else {
    const options = key as CreateAsymmetricKeyOptions;
    if (typeof options.key === 'string') {
      inner.key = Buffer.from(options.key as string, options.encoding);
    } else if (isArrayBufferView(options.key)) {
      inner.key = options.key as ArrayBufferView;
    } else if (isArrayBuffer(options.key)) {
      inner.key = options.key as ArrayBuffer;
    } else if (isSharedArrayBuffer(options.key)) {
      inner.key = options.key as SharedArrayBuffer;
    } else if (type === kPublicKeyTag && isKeyObject(key)) {
      if (!isPrivateKeyObject(key)) {
        throw new ERR_INVALID_ARG_VALUE('options.key', options.key, 'must be a private key');
      }
      inner.key = (options.key as PrivateKeyObject)[kHandle];
    } else if (type === kPublicKeyTag && key instanceof CryptoKey) {
      if ((options.key as CryptoKey).type !== 'private') {
        throw new ERR_INVALID_ARG_VALUE('options.key', options.key, 'must be a private key');
      }
      inner.key = options.key as CryptoKey;
    } else {
      inner.key = key as JsonWebKey;
    }
    validateKeyData(inner.key, 'options.key', { allowObject: true });

    if (options.format !== undefined) {
      validateString(options.format, 'options.format');
      inner.format = options.format;
    }
    if (options.type !== undefined) {
      validateString(options.type, 'options.type');
      inner.type = options.type;
    }
    if (options.passphrase !== undefined) {
      if (typeof options.passphrase === 'string') {
        inner.passphrase = Buffer.from(options.passphrase, options.encoding);
      } else {
        if (!isUint8Array(options.passphrase)) {
          throw new ERR_INVALID_ARG_TYPE('options.passphrase', [
            'string', 'Uint8Array'
          ], options.passphrase);
        }
        inner.passphrase = options.passphrase;
      }
      if (inner.passphrase.byteLength > 1024) {
        throw new ERR_INVALID_ARG_VALUE('options.passphrase', options.passphrase.length, '<= 1024');
      }
    }
  }
  return inner;
}

export function createPrivateKey(key: string) : PrivateKeyObject;
export function createPrivateKey(key: ArrayBuffer | ArrayBufferView) : PrivateKeyObject;
export function createPrivateKey(key: CreateAsymmetricKeyOptions) : PrivateKeyObject;
export function createPrivateKey(key: CreateAsymmetricKeyOptions | KeyData) : PrivateKeyObject {
  // The options here are fairly complex. The key data can be a string,
  // ArrayBuffer, or ArrayBufferView. The first argument can be one of
  // these or an object with a key property that is one of these. If the
  // key data is a string, then it will be decoded using an encoding
  // (defaults to UTF8).
  const inner = validateAsymmetricKeyOptions(key, kPrivateKeyTag);
  return new PrivateKeyObject(kSkipThrow, new AsymmetricKeyObjectHandle(inner));
}

export function createPublicKey(key: string) : PublicKeyObject;
export function createPublicKey(key: ArrayBuffer) : PublicKeyObject;
export function createPublicKey(key: ArrayBufferView) : PublicKeyObject;

export function createPublicKey(key: PrivateKeyObject | PublicKeyObject) : PublicKeyObject;
export function createPublicKey(key: CryptoKey) : PublicKeyObject;
export function createPublicKey(key: CreateAsymmetricKeyOptions) : PublicKeyObject;
export function createPublicKey(key: CreateAsymmetricKeyOptions | KeyData | CryptoKey | PrivateKeyObject | PublicKeyObject)
    : PublicKeyObject {
  if ((key as any)?.[kPublicKeyTag] === kPublicKeyTag) return key as PublicKeyObject;

  // The options here are a bit complicated. The key material itself can
  // either be a string, ArrayBuffer, or ArrayBufferView. It is also
  // possible to pass a private key in the form of either a CryptoKey
  // or KeyObject. The first argument can be one of these, or an object
  // whose key value is one of these. If the key data is a string, then
  // it will be decoded using an encoding (defaults to UTF8). If a
  // CryptoKey or KeyObject is passed, it will be used to derived the
  // public key.
  const inner = validateAsymmetricKeyOptions(key as KeyDataToValidate, kPublicKeyTag);
  return new PublicKeyObject(kSkipThrow, new AsymmetricKeyObjectHandle(inner));
}

// ======================================================================================

export type PublicKeyResult = KeyExportResult | PublicKeyObject;
export type PrivateKeyResult = KeyExportResult | PrivateKeyObject;
export type GenerateKeyCallback = (err?: any, key?: KeyObject) => void;
export type GenerateKeyPairCallback =
  (err?: any, publicKey?: PublicKeyResult, privateKey?: PrivateKeyResult) => void;

export interface KeyObjectPair {
  publicKey: PublicKeyResult;
  privateKey: PrivateKeyResult;
}

export function generateKey(_type: SecretKeyType,
  _options: GenerateKeyOptions,
  callback: GenerateKeyCallback) {
// We intentionally have not implemented key generation up to this point.
// The reason is that generation of cryptographically safe keys is a CPU
// intensive operation that can often exceed limits on the amount of CPU
// time a worker is allowed.
callback(new ERR_METHOD_NOT_IMPLEMENTED('crypto.generateKeySync'));
}

export function generateKeySync(_type: SecretKeyType,
      _options: GenerateKeyOptions) {
// We intentionally have not implemented key generation up to this point.
// The reason is that generation of cryptographically safe keys is a CPU
// intensive operation that can often exceed limits on the amount of CPU
// time a worker is allowed.
throw new ERR_METHOD_NOT_IMPLEMENTED('crypto.generateKeySync');
}

export function generateKeyPair(
    _type : AsymmetricKeyType,
    _options: GenerateKeyPairOptions,
    callback: GenerateKeyPairCallback) {
  // We intentionally have not implemented key generation up to this point.
  // The reason is that generation of cryptographically safe keys is a CPU
  // intensive operation that can often exceed limits on the amount of CPU
  // time a worker is allowed.
  callback(new ERR_METHOD_NOT_IMPLEMENTED('crypto.generateKeyPair'));
}

export function generateKeyPairSync(
    _type : AsymmetricKeyType,
    _options: GenerateKeyPairOptions) : KeyObjectPair {
  // We intentionally have not implemented key generation up to this point.
  // The reason is that generation of cryptographically safe keys is a CPU
  // intensive operation that can often exceed limits on the amount of CPU
  // time a worker is allowed.
  throw new ERR_METHOD_NOT_IMPLEMENTED('crypto.generateKeyPairSync');
}
