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
  InnerExportOptions,
  // TODO(soon): Uncomment these once createPrivateKey/createPublicKey are implemented.
  // JsonWebKey,
  // InnerCreateAsymmetricKeyOptions,
  default as cryptoImpl,
} from 'node-internal:crypto';

import {
  arrayBufferToUnsignedBigInt,
  kHandle,
} from 'node-internal:crypto_util';

import {
  isAnyArrayBuffer,
  isArrayBuffer,
  isArrayBufferView,
  isUint8Array,
  // TODO(soon): Uncomment these once createPrivateKey/createPublicKey are implemented.
  // isSharedArrayBuffer,
} from 'node-internal:internal_types';

import {
  ERR_INVALID_ARG_TYPE,
  ERR_METHOD_NOT_IMPLEMENTED,
  // TODO(soon): Uncomment these once createPrivateKey/createPublicKey are implemented.
  // ERR_INVALID_ARG_VALUE,
} from 'node-internal:internal_errors';

import { validateObject, validateString } from 'node-internal:validators';

// In Node.js, the definition of KeyObject is a bit complicated because
// KeyObject instances in Node.js can be transferred via postMessage() and
// structuredClone(), etc, allowing instances to be shared across multiple
// worker threads. We do not implement that model so we're essentially
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

  static from(key: CryptoKey): KeyObject {
    if (!(key instanceof CryptoKey)) {
      throw new ERR_INVALID_ARG_TYPE('key', 'CryptoKey', key);
    }
    switch (key.type) {
      case 'secret':
        return Reflect.construct(
          function (this: SecretKeyObject) {
            this[kHandle] = key;
          },
          [],
          SecretKeyObject
        );
      case 'private':
        return Reflect.construct(
          function (this: PrivateKeyObject) {
            this[kHandle] = key;
          },
          [],
          PrivateKeyObject
        );
      case 'public':
        return Reflect.construct(
          function (this: PublicKeyObject) {
            this[kHandle] = key;
          },
          [],
          PublicKeyObject
        );
    }
  }

  export(options: ExportOptions = {}): KeyExportResult {
    validateObject(options, 'options', {});

    // Yes, converting to any is a bit of a cheat, but it allows us to check
    // each option individually without having to do a bunch of type guards.
    const opts = options as any;
    if (opts.format !== undefined)
      validateString(opts.format, 'options.format');
    if (opts.type !== undefined) validateString(opts.type, 'options.type');
    if (this.type === 'private') {
      if (opts.cipher !== undefined) {
        validateString(opts.cipher, 'options.cipher');
        if (typeof opts.passphrase === 'string') {
          opts.passphrase = Buffer.from(opts.passphrase, opts.encoding);
        }
        if (!isUint8Array(opts.passphrase)) {
          throw new ERR_INVALID_ARG_TYPE(
            'options.passphrase',
            ['string', 'Uint8Array'],
            opts.passphrase
          );
        }
      }
    }

    const ret = cryptoImpl.exportKey(
      this[kHandle],
      options as InnerExportOptions
    );
    if (typeof ret === 'string') return ret;
    if (isUint8Array(ret)) {
      return Buffer.from(
        (ret as Uint8Array).buffer,
        ret.byteOffset,
        ret.byteLength
      ) as KeyExportResult;
    } else if (isArrayBuffer(ret)) {
      return Buffer.from(
        ret as ArrayBuffer,
        0,
        (ret as ArrayBuffer).byteLength
      );
    }
    return ret;
  }

  equals(otherKeyObject: KeyObject): boolean {
    if (this === otherKeyObject || this[kHandle] === otherKeyObject[kHandle])
      return true;
    if (this.type !== otherKeyObject.type) return false;
    if (!(otherKeyObject[kHandle] instanceof CryptoKey)) {
      throw new ERR_INVALID_ARG_TYPE(
        'otherKeyObject',
        'KeyObject',
        otherKeyObject
      );
    }
    return cryptoImpl.equals(this[kHandle], otherKeyObject[kHandle]);
  }

  abstract get type(): KeyObjectType;

  get [Symbol.toStringTag]() {
    return 'KeyObject';
  }
}

abstract class AsymmetricKeyObject extends KeyObject {
  get asymmetricKeyDetails(): AsymmetricKeyDetails {
    let detail = cryptoImpl.getAsymmetricKeyDetail(this[kHandle]);
    if (isArrayBuffer(detail.publicExponent)) {
      detail.publicExponent = arrayBufferToUnsignedBigInt(
        detail.publicExponent as any
      );
    }
    return detail;
  }

  get asymmetricKeyType(): AsymmetricKeyType {
    return cryptoImpl.getAsymmetricKeyType(this[kHandle]);
  }
}

export class PublicKeyObject extends AsymmetricKeyObject {
  override export(options?: PublicKeyExportOptions): KeyExportResult {
    return super.export(options);
  }

  get type(): KeyObjectType {
    return 'public';
  }
}

export class PrivateKeyObject extends AsymmetricKeyObject {
  override export(options?: PrivateKeyExportOptions): KeyExportResult {
    return super.export(options);
  }

  get type(): KeyObjectType {
    return 'private';
  }
}

export class SecretKeyObject extends KeyObject {
  get symmetricKeySize(): number {
    return (this[kHandle].algorithm as any).length | 0;
  }

  override export(options?: SecretKeyExportOptions): KeyExportResult {
    return super.export(options);
  }

  get type(): KeyObjectType {
    return 'secret';
  }
}

type ValidateKeyDataOptions = {
  allowObject?: boolean;
};
function validateKeyData(
  key: unknown,
  name: string,
  options: ValidateKeyDataOptions = {
    allowObject: false,
  }
) {
  if (
    key == null ||
    (typeof key !== 'string' &&
      options.allowObject &&
      typeof key !== 'object' &&
      !isArrayBufferView(key) &&
      !isAnyArrayBuffer(key))
  ) {
    const expected = ['string', 'ArrayBuffer', 'TypedArray', 'DataView'];
    if (options.allowObject) expected.push('object');
    throw new ERR_INVALID_ARG_TYPE(name, expected, key);
  }
}

export function createSecretKey(
  key: string,
  encoding?: string
): SecretKeyObject;
export function createSecretKey(
  key: ArrayBuffer | ArrayBufferView
): SecretKeyObject;
export function createSecretKey(
  key: KeyData,
  encoding?: string
): SecretKeyObject {
  validateKeyData(key, 'key');
  if (typeof key === 'string') key = Buffer.from(key as string, encoding);
  return KeyObject.from(cryptoImpl.createSecretKey(key)) as SecretKeyObject;
}

// TODO(soon): Fully implement createPrivateKey/createPublicKey. These are the
// equivalent of the WebCrypto API's importKey() method but operate synchronously
// and support a range of options not currently supported by WebCrypto. Implementing
// these will require either duplicating or significantly refactoring the current
// import key logic that supports Web Crypto now as the import logic is spread out
// over several locations and makes a number of assumptions that Web Crypto is being
// used.
//
// For now, users can use Web Crypto to import a CryptoKey then convert that into
// a KeyObject using KeyObject.from().
//
// const kPrivateKey = Symbol('privateKey');
// const kPublicKey = Symbol('publicKey');

// function validateAsymmetricKeyOptions(
//     key: CreateAsymmetricKeyOptions | KeyData | CryptoKey | KeyObject,
//     type: Symbol) {
//   validateKeyData(key, 'key', { allowObject: true });
//   let inner : InnerCreateAsymmetricKeyOptions = {};
//   inner.format = 'pem';
//   if (typeof key === 'string') {
//     inner.key = Buffer.from(key as string);
//   } else if (isArrayBufferView(key)) {
//     inner.key = key as ArrayBufferView;
//   } else if (isArrayBuffer(key)) {
//     inner.key = key as ArrayBuffer;
//   } else if (isSharedArrayBuffer(key)) {
//     inner.key = key as SharedArrayBuffer;
//   } else if (type === kPublicKey && key instanceof KeyObject) {
//     // Covers deriving public key from a private key.
//     if (key.type !== 'private') {
//       throw new ERR_INVALID_ARG_VALUE('key', key, 'must be a private key');
//     }
//     inner.key = (key as KeyObject)[kHandle];
//   } else if (type === kPublicKey && key instanceof CryptoKey) {
//     // Covers deriving public key from a private key.
//     if ((key as CryptoKey).type !== 'private') {
//       throw new ERR_INVALID_ARG_VALUE('key', key, 'must be a private key');
//     }
//     inner.key = key as CryptoKey;
//   } else {
//     const options = key as CreateAsymmetricKeyOptions;
//     if (typeof options.key === 'string') {
//       inner.key = Buffer.from(options.key as string, options.encoding);
//     } else if (isArrayBufferView(options.key)) {
//       inner.key = options.key as ArrayBufferView;
//     } else if (isArrayBuffer(options.key)) {
//       inner.key = options.key as ArrayBuffer;
//     } else if (isSharedArrayBuffer(options.key)) {
//       inner.key = options.key as SharedArrayBuffer;
//     } else if (type === kPublicKey && key instanceof KeyObject) {
//       if ((options.key as KeyObject).type !== 'private') {
//         throw new ERR_INVALID_ARG_VALUE('options.key', options.key, 'must be a private key');
//       }
//       inner.key = (options.key as KeyObject)[kHandle];
//     } else if (type === kPublicKey && key instanceof CryptoKey) {
//       if ((options.key as CryptoKey).type !== 'private') {
//         throw new ERR_INVALID_ARG_VALUE('options.key', options.key, 'must be a private key');
//       }
//       inner.key = options.key as CryptoKey;
//     } else {
//       inner.key = key as JsonWebKey;
//     }
//     validateKeyData(inner.key, 'options.key', { allowObject: true });

//     if (options.format !== undefined) {
//       validateString(options.format, 'options.format');
//       inner.format = options.format;
//     }
//     if (options.type !== undefined) {
//       validateString(options.type, 'options.type');
//       inner.type = options.type;
//     }
//     if (options.passphrase !== undefined) {
//       if (typeof options.passphrase === 'string') {
//         inner.passphrase = Buffer.from(options.passphrase, options.encoding);
//       } else {
//         if (!isUint8Array(options.passphrase)) {
//           throw new ERR_INVALID_ARG_TYPE('options.passphrase', [
//             'string', 'Uint8Array'
//           ], options.passphrase);
//         }
//         inner.passphrase = options.passphrase;
//       }
//       if (inner.passphrase.byteLength > 1024) {
//         throw new ERR_INVALID_ARG_VALUE('options.passphrase', options.passphrase.length, '<= 1024');
//       }
//     }
//   }
//   return inner;
// }

export function createPrivateKey(key: string): PrivateKeyObject;
export function createPrivateKey(
  key: ArrayBuffer | ArrayBufferView
): PrivateKeyObject;
export function createPrivateKey(
  key: CreateAsymmetricKeyOptions
): PrivateKeyObject;
export function createPrivateKey(
  _key: CreateAsymmetricKeyOptions | KeyData
): PrivateKeyObject {
  // The options here are fairly complex. The key data can be a string,
  // ArrayBuffer, or ArrayBufferView. The first argument can be one of
  // these or an object with a key property that is one of these. If the
  // key data is a string, then it will be decoded using an encoding
  // (defaults to UTF8).
  throw new ERR_METHOD_NOT_IMPLEMENTED('crypto.createPrivateKey');
  // return KeyObject.from(cryptoImpl.createPrivateKey(
  //     validateAsymmetricKeyOptions(key, kPrivateKey))) as PrivateKeyObject;
}

export function createPublicKey(key: string): PublicKeyObject;
export function createPublicKey(key: ArrayBuffer): PublicKeyObject;
export function createPublicKey(key: ArrayBufferView): PublicKeyObject;

export function createPublicKey(key: KeyObject): PublicKeyObject;
export function createPublicKey(key: CryptoKey): PublicKeyObject;
export function createPublicKey(
  key: CreateAsymmetricKeyOptions
): PublicKeyObject;
export function createPublicKey(
  _key: CreateAsymmetricKeyOptions | KeyData | CryptoKey | KeyObject
): PublicKeyObject {
  // The options here are a bit complicated. The key material itself can
  // either be a string, ArrayBuffer, or ArrayBufferView. It is also
  // possible to pass a private key in the form of either a CryptoKey
  // or KeyObject. The first argument can be one of these, or an object
  // whose key value is one of these. If the key data is a string, then
  // it will be decoded using an encoding (defaults to UTF8). If a
  // CryptoKey or KeyObject is passed, it will be used to derived the
  // public key.
  throw new ERR_METHOD_NOT_IMPLEMENTED('crypto.createPublicKey');
  // return KeyObject.from(cryptoImpl.createPublicKey(
  //     validateAsymmetricKeyOptions(key, kPublicKey))) as PublicKeyObject;
}

// ======================================================================================

export type PublicKeyResult = KeyExportResult | PublicKeyObject;
export type PrivateKeyResult = KeyExportResult | PrivateKeyObject;
export type GenerateKeyCallback = (err?: any, key?: KeyObject) => void;
export type GenerateKeyPairCallback = (
  err?: any,
  publicKey?: PublicKeyResult,
  privateKey?: PrivateKeyResult
) => void;

export interface KeyObjectPair {
  publicKey: PublicKeyResult;
  privateKey: PrivateKeyResult;
}

export function generateKey(
  _type: SecretKeyType,
  _options: GenerateKeyOptions,
  callback: GenerateKeyCallback
) {
  // We intentionally have not implemented key generation up to this point.
  // The reason is that generation of cryptographically safe keys is a CPU
  // intensive operation that can often exceed limits on the amount of CPU
  // time a worker is allowed.
  callback(new ERR_METHOD_NOT_IMPLEMENTED('crypto.generateKeySync'));
}

export function generateKeySync(
  _type: SecretKeyType,
  _options: GenerateKeyOptions
) {
  // We intentionally have not implemented key generation up to this point.
  // The reason is that generation of cryptographically safe keys is a CPU
  // intensive operation that can often exceed limits on the amount of CPU
  // time a worker is allowed.
  throw new ERR_METHOD_NOT_IMPLEMENTED('crypto.generateKeySync');
}

export function generateKeyPair(
  _type: AsymmetricKeyType,
  _options: GenerateKeyPairOptions,
  callback: GenerateKeyPairCallback
) {
  // We intentionally have not implemented key generation up to this point.
  // The reason is that generation of cryptographically safe keys is a CPU
  // intensive operation that can often exceed limits on the amount of CPU
  // time a worker is allowed.
  callback(new ERR_METHOD_NOT_IMPLEMENTED('crypto.generateKeyPair'));
}

export function generateKeyPairSync(
  _type: AsymmetricKeyType,
  _options: GenerateKeyPairOptions
): KeyObjectPair {
  // We intentionally have not implemented key generation up to this point.
  // The reason is that generation of cryptographically safe keys is a CPU
  // intensive operation that can often exceed limits on the amount of CPU
  // time a worker is allowed.
  throw new ERR_METHOD_NOT_IMPLEMENTED('crypto.generateKeyPairSync');
}
