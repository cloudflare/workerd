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
import { randomBytes } from 'node-internal:crypto_random';
import {
  CryptoKey,
  CryptoKeyPair,
  JsonWebKey,
  KeyData,
  KeyObjectType,
  KeyExportResult,
  SecretKeyType,
  SecretKeyExportOptions,
  PublicKeyExportOptions,
  PrivateKeyExportOptions,
  ExportOptions,
  AsymmetricKeyDetails,
  AsymmetricKeyType,
  CreateAsymmetricKeyOptions,
  GenerateKeyOptions,
  GenerateKeyPairOptions,
  InnerCreateAsymmetricKeyOptions,
  InnerExportOptions,
  default as cryptoImpl
} from 'node-internal:crypto';

import {
  arrayBufferToUnsignedBigInt,
  kHandle,
} from 'node-internal:crypto_util';

import {
  isAnyArrayBuffer,
  isArrayBuffer,
  isSharedArrayBuffer,
  isArrayBufferView,
  isUint8Array,
} from 'node-internal:internal_types';

import {
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_ARG_VALUE,
} from 'node-internal:internal_errors';

import {
  validateFunction,
  validateInteger,
  validateObject,
  validateOneOf,
  validateString,
  validateUint32,
} from 'node-internal:validators';

// In Node.js, the definition of KeyObject is a bit complicated because
// KeyObject instances in Node.js can be transfered via postMessage() and
// structuredClone(), etc, allowing instances to be shared across multiple
// worker threads. We do not implement that model so we're esssentially
// re-implementing the Node.js API here instead of just taking their code.
// Also in Node.js, CryptoKey is layered on top of KeyObject since KeyObject
// existed first. We're, however, going to layer our KeyObject on top of
// CryptoKey with a few augmentations.

export abstract class KeyObject {
  [kHandle]: CryptoKey;

  constructor() {
    // KeyObjects cannot be created with new ... use one of the
    // create or generate methods, or use from to get from a
    // CryptoKey.
    throw new Error('Illegal constructor');
  }

  static from(key: CryptoKey) : KeyObject {
    if (!(key instanceof CryptoKey)) {
      throw new ERR_INVALID_ARG_TYPE('key', 'CryptoKey', key);
    }
    switch (key.type) {
      case 'secret':
        return Reflect.construct(function(this: SecretKeyObject) {
          this[kHandle] = key;
        }, [], SecretKeyObject);
      case 'private':
        return Reflect.construct(function(this: PrivateKeyObject) {
          this[kHandle] = key;
        }, [], PrivateKeyObject);
      case 'public':
        return Reflect.construct(function(this: PublicKeyObject) {
          this[kHandle] = key;
        }, [], PublicKeyObject);
    }
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

    const ret = cryptoImpl.exportKey(this[kHandle], options as InnerExportOptions);
    if (typeof ret === 'string') return ret;
    if (isUint8Array(ret)) {
      return Buffer.from((ret as Uint8Array).buffer, ret.byteOffset, ret.byteLength) as KeyExportResult;
    } else if (isArrayBuffer(ret)) {
      return Buffer.from(ret as ArrayBuffer, 0, (ret as ArrayBuffer).byteLength);
    }
    return ret;
  }

  equals(otherKeyObject: KeyObject) : boolean {
    if (this === otherKeyObject ||
        this[kHandle] === otherKeyObject[kHandle]) return true;
    if (this.type !== otherKeyObject.type) return false;
    if (!(otherKeyObject[kHandle] instanceof CryptoKey)) {
      throw new ERR_INVALID_ARG_TYPE('otherKeyObject', 'KeyObject', otherKeyObject);
    }
    return cryptoImpl.equals(this[kHandle], otherKeyObject[kHandle]);
  }

  abstract get type() : KeyObjectType;
}

abstract class AsymmetricKeyObject extends KeyObject {
  get asymmetricKeyDetails() : AsymmetricKeyDetails {
    let detail = cryptoImpl.getAsymmetricKeyDetail(this[kHandle]);
    if (isArrayBuffer(detail.publicExponent)) {
      detail.publicExponent = arrayBufferToUnsignedBigInt(detail.publicExponent as any);
    }
    return detail;
  }

  get asymmetricKeyType() : AsymmetricKeyType {
    return cryptoImpl.getAsymmetricKeyType(this[kHandle]);
  }
}

export class PublicKeyObject extends AsymmetricKeyObject {
  override export(options?: PublicKeyExportOptions) : KeyExportResult {
    return super.export(options);
  }

  get type() : KeyObjectType { return 'public'; }
}

export class PrivateKeyObject extends AsymmetricKeyObject {
  override export(options?: PrivateKeyExportOptions) : KeyExportResult {
    return super.export(options);
  }

  get type() : KeyObjectType { return 'private'; }
}

export class SecretKeyObject extends KeyObject {
  get symmetricKeySize() : number {
    return (this[kHandle].algorithm as any).length | 0
  }

  override export(options?: SecretKeyExportOptions) : KeyExportResult {
    return super.export(options);
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
  return KeyObject.from(cryptoImpl.createSecretKey(key)) as SecretKeyObject;
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
  validateKeyData(key, 'key', { allowObject: true });
  let inner : InnerCreateAsymmetricKeyOptions = {};
  if (typeof key === 'string') {
    inner.key = Buffer.from(key as string);
    inner.format = 'pem';
  } else if (isArrayBufferView(key)) {
    inner.key = key as ArrayBufferView;
    inner.format = 'pem';
  } else if (isArrayBuffer(key)) {
    inner.key = key as ArrayBuffer;
    inner.format = 'pem';
  } else if (isSharedArrayBuffer(key)) {
    inner.key = key as SharedArrayBuffer;
    inner.format = 'pem';
  } else {
    const options = key as CreateAsymmetricKeyOptions;
    if (typeof options.key === 'string') {
      inner.key = Buffer.from(options.key as string, options.encoding);
      inner.format = 'pem';
    } else if (isArrayBufferView(options.key)) {
      inner.key = options.key as ArrayBufferView;
      inner.format = 'pem';
    } else if (isArrayBuffer(options.key)) {
      inner.key = options.key as ArrayBuffer;
      inner.format = 'pem';
    } else if (isSharedArrayBuffer(options.key)) {
      inner.key = options.key as SharedArrayBuffer;
      inner.format = 'pem';
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
    }
  }
  return KeyObject.from(cryptoImpl.createPrivateKey(inner)) as PrivateKeyObject;
}

export function createPublicKey(key: string) : PublicKeyObject;
export function createPublicKey(key: ArrayBuffer) : PublicKeyObject;
export function createPublicKey(key: ArrayBufferView) : PublicKeyObject;
export function createPublicKey(key: KeyObject) : PublicKeyObject;
export function createPublicKey(key: CryptoKey) : PublicKeyObject;
export function createPublicKey(key: CreateAsymmetricKeyOptions) : PublicKeyObject;
export function createPublicKey(key: CreateAsymmetricKeyOptions | KeyData | CryptoKey | KeyObject)
    : PublicKeyObject {
  // The options here are a bit complicated. The key material itself can
  // either be a string, ArrayBuffer, or ArrayBufferView. It is also
  // possible to pass a private key in the form of either a CryptoKey
  // or KeyObject. The first argument can be one of these, or an object
  // whose key value is one of these. If the key data is a string, then
  // it will be decoded using an encoding (defaults to UTF8). If a
  // CryptoKey or KeyObject is passed, it will be used to derived the
  // public key.
  validateKeyData(key, 'key', { allowObject: true });
  const inner : InnerCreateAsymmetricKeyOptions = {};
  if (typeof key === 'string') {
    inner.key = Buffer.from(key as string);
    inner.format = 'pem';
  } else if (isArrayBufferView(key)) {
    inner.key = key as ArrayBufferView;
    inner.format = 'pem';
  } else if (isArrayBuffer(key)) {
    inner.key = key as ArrayBuffer;
    inner.format = 'pem';
  } else if (isSharedArrayBuffer(key)) {
    inner.key = key as SharedArrayBuffer;
    inner.format = 'pem';
  } else if (key instanceof KeyObject) {
    if (key.type !== 'private') {
      throw new ERR_INVALID_ARG_VALUE('key', key, 'must be a private key');
    }
    inner.key = (key as KeyObject)[kHandle];
  } else if (key instanceof CryptoKey) {
    if ((key as CryptoKey).type !== 'private') {
      throw new ERR_INVALID_ARG_VALUE('key', key, 'must be a private key');
    }
    inner.key = key as CryptoKey;
  } else {
    const options = key as CreateAsymmetricKeyOptions;
    if (typeof options.key === 'string') {
      inner.key = Buffer.from(options.key as string, options.encoding);
      inner.format = 'pem';
    } else if (isArrayBufferView(options.key)) {
      inner.key = options.key as ArrayBufferView;
      inner.format = 'pem';
    } else if (isArrayBuffer(options.key)) {
      inner.key = options.key as ArrayBuffer;
      inner.format = 'pem';
    } else if (isSharedArrayBuffer(options.key)) {
      inner.key = options.key as SharedArrayBuffer;
      inner.format = 'pem';
    } else if (options.key instanceof KeyObject) {
      if (options.key.type !== 'private') {
        throw new ERR_INVALID_ARG_VALUE('options.key', options.key, 'must be a private key');
      }
      inner.key = (options.key as KeyObject)[kHandle];
    } else if (options.key instanceof CryptoKey) {
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
    }
  }

  return KeyObject.from(cryptoImpl.createPublicKey(inner)) as PublicKeyObject;
}

export type GenerateKeyCallback = (err?: any, key?: KeyObject) => void;

function validateGenerateKeyOptions(type: SecretKeyType, options: GenerateKeyOptions) {
  // While Node.js requires that we pass in a type, it uses that
  // only for validation of the length. The secret key returned
  // is not specific to either hmac or aes.
  validateOneOf(type, 'type', ['hmac', 'aes']);
  validateObject(options, 'options', {});
  const { length } = options;
  validateUint32(length, 'options.length');
  if (type === 'aes') {
    validateOneOf(length, 'options.length', [128, 192, 256]);
  } else if (type === 'hmac') {
    validateInteger(length, 'options.length', 8, 2147483647);
  } else {
    throw new ERR_INVALID_ARG_VALUE('type', type);
  }
}

export function generateKey(type: SecretKeyType,
                            options: GenerateKeyOptions,
                            callback: GenerateKeyCallback) {
  validateGenerateKeyOptions(type, options);
  validateFunction(callback, 'callback');
  new Promise<KeyObject>((resolve, reject) => {
    try {
      resolve(KeyObject.from(cryptoImpl.createSecretKey(randomBytes(options.length))));
    } catch (err) {
      reject(err);
    }
  }).then((key: KeyObject) => callback(null, key))
    .catch((err: any) => callback(err));
}

export function generateKeySync(type: SecretKeyType,
                                options: GenerateKeyOptions) {
  validateGenerateKeyOptions(type, options);
  return KeyObject.from(cryptoImpl.createSecretKey(randomBytes(options.length)));
}

export type PublicKeyResult = KeyExportResult | PublicKeyObject;
export type PrivateKeyResult = KeyExportResult | PrivateKeyObject;
export type GenerateKeyPairCallback =
  (err?: any, publicKey?: PublicKeyResult, privateKey?: PrivateKeyResult) => void;

export interface KeyObjectPair {
  publicKey: PublicKeyResult;
  privateKey: PrivateKeyResult;
}

function validateGenerateKeyPairOptions(
    type : AsymmetricKeyType,
    options: GenerateKeyPairOptions) {
  validateOneOf(type, 'type', ['rsa', 'rsa-pss', 'dsa', 'ec', 'x25519', 'ed25519', 'dh']);
  validateObject(options, 'options', {});
  const {
    modulusLength,
    publicExponent,
    hashAlgorithm,
    mgf1HashAlgorithm,
    saltLength,
    divisorLength,
    namedCurve,
    prime,
    primeLength,
    generator,
    groupName,
    paramEncoding,
    publicKeyEncoding,
    privateKeyEncoding,
  } = options;
  if (modulusLength !== undefined) validateInteger(modulusLength, 'options.modulusLength', 0);
  if (publicExponent !== undefined) {
    if (typeof publicExponent !== 'bigint' && typeof publicExponent !== 'number')
      throw new ERR_INVALID_ARG_TYPE('options.publicExponent', ['bigint', 'number'], publicExponent);
  }
  if (hashAlgorithm !== undefined) {
    validateString(hashAlgorithm, 'options.hashAlgorithm');
  }
  if (mgf1HashAlgorithm !== undefined) {
    validateString(mgf1HashAlgorithm, 'options.mgf1HashAlgorithm');
  }
  if (saltLength !== undefined) {
    validateInteger(saltLength, 'options.saltLength', 0);
  }
  if (divisorLength !== undefined) {
    validateInteger(divisorLength, 'options.divisorLength', 0);
  }
  if (namedCurve !== undefined) {
    validateString(namedCurve, 'options.namedCurve');
  }
  if (prime !== undefined) {
    if (!isUint8Array(prime))
      throw new ERR_INVALID_ARG_TYPE('options.prime', 'Uint8Array', prime);
  }
  if (primeLength !== undefined) {
    validateInteger(primeLength, 'options.primeLength', 0);
  }
  if (generator !== undefined) {
    validateInteger(generator, 'options.generator', 0);
  }
  if (groupName !== undefined) {
    validateString(groupName, 'options.groupName');
  }
  if (paramEncoding !== undefined) {
    validateOneOf(paramEncoding, 'options.paramEncoding', ['named', 'explicit']);
  }
  if (publicKeyEncoding !== undefined) {
    validateObject(publicKeyEncoding, 'options.publicKeyEncoding', {});
  }
  if (privateKeyEncoding !== undefined) {
    validateObject(privateKeyEncoding, 'options.privateKeyEncoding', {});
  }
}

function toExportedKeyPair(inner : CryptoKeyPair, options: GenerateKeyPairOptions) : KeyObjectPair {
  const {
    publicKey: pubkey,
    privateKey: pvtkey,
  } = inner;

  let publicKey: PublicKeyResult = KeyObject.from(pubkey) as PublicKeyObject;
  let privateKey: PrivateKeyResult = KeyObject.from(pvtkey) as PrivateKeyObject;

  if (options.publicKeyEncoding !== undefined) {
    publicKey = publicKey.export(options.publicKeyEncoding);
  }
  if (options.privateKeyEncoding !== undefined) {
    privateKey = privateKey.export(options.privateKeyEncoding);
  }

  return { publicKey, privateKey };
}

export function generateKeyPair(
    type : AsymmetricKeyType,
    options: GenerateKeyPairOptions,
    callback: GenerateKeyPairCallback) {
  validateGenerateKeyPairOptions(type, options);
  validateFunction(callback, 'callback');
  new Promise<KeyObjectPair>((resolve, reject) => {
    try {
      resolve(toExportedKeyPair(cryptoImpl.generateKeyPair(type, options), options));
    } catch (err) {
      reject(err);
    }
  }).then(({ publicKey, privateKey }) => callback(null, publicKey, privateKey))
    .catch((err: any) => callback(err));
}

export function generateKeyPairSync(
    type : AsymmetricKeyType,
    options: GenerateKeyPairOptions) : KeyObjectPair {
  validateGenerateKeyPairOptions(type, options);
  return toExportedKeyPair(cryptoImpl.generateKeyPair(type, options), options);
}
