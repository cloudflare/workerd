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

/* TODO: the following is adopted code, enabling linting one day */
/* eslint-disable */

import { Buffer } from 'node-internal:internal_buffer';

import {
  type KeyData,
  type KeyObjectType,
  type KeyExportResult,
  type SecretKeyType,
  type SecretKeyExportOptions,
  type PublicKeyExportOptions,
  type PrivateKeyExportOptions,
  type ExportOptions,
  type AsymmetricKeyDetails,
  type AsymmetricKeyType,
  type CreateAsymmetricKeyOptions,
  type GenerateKeyOptions,
  type GenerateKeyPairOptions,
  type InnerExportOptions,
  type InnerCreateAsymmetricKeyOptions,
  type JsonWebKey,
  default as cryptoImpl,
} from 'node-internal:crypto';

import {
  arrayBufferToUnsignedBigInt,
  getArrayBufferOrView,
  kHandle,
} from 'node-internal:crypto_util';

import {
  isAnyArrayBuffer,
  isArrayBuffer,
  isArrayBufferView,
  isUint8Array,
} from 'node-internal:internal_types';

import {
  ERR_INVALID_ARG_TYPE,
  ERR_METHOD_NOT_IMPLEMENTED,
} from 'node-internal:internal_errors';

import {
  validateObject,
  validateOneOf,
  validateString,
} from 'node-internal:validators';

import { inspect } from 'node-internal:internal_inspect';
const kInspect = inspect.custom;

// Key input contexts.
enum KeyContext {
  kConsumePublic,
  kConsumePrivate,
  kCreatePublic,
  kCreatePrivate,
}

// In Node.js, the definition of KeyObject is a bit complicated because
// KeyObject instances in Node.js can be transferred via postMessage() and
// structuredClone(), etc, allowing instances to be shared across multiple
// worker threads. We do not implement that model so we're essentially
// re-implementing the Node.js API here instead of just taking their code.
// Also in Node.js, CryptoKey is layered on top of KeyObject since KeyObject
// existed first. We're, however, going to layer our KeyObject on top of
// CryptoKey with a few augmentations.

function isStringOrBuffer(val: any) {
  return (
    typeof val === 'string' || isArrayBufferView(val) || isAnyArrayBuffer(val)
  );
}

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
    if (opts.format !== undefined) {
      validateString(opts.format, 'options.format');
    }
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

    options.format ??= 'buffer';

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

export function isKeyObject(obj: any): obj is KeyObject {
  return obj[kHandle] !== undefined;
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

  toCryptoKey() {
    // TODO(soon): Implement the toCryptoKey API (added in Node.js 23.0.0)
    throw new ERR_METHOD_NOT_IMPLEMENTED('toCryptoKey');
  }

  [kInspect](depth: number, options: any) {
    if (depth < 0) return this;

    const opts = {
      ...options,
      depth: options.depth == null ? null : options.depth - 1,
    };

    return `${this.constructor.name} ${inspect(
      {
        type: this.asymmetricKeyType,
        details: this.asymmetricKeyDetails,
      },
      opts
    )}`;
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

  [kInspect](depth: number, options: any) {
    if (depth < 0) return this;

    const opts = {
      ...options,
      depth: options.depth == null ? null : options.depth - 1,
    };

    return `${this.constructor.name} ${inspect(
      {
        size: this.symmetricKeySize,
      },
      opts
    )}`;
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
  if (typeof key === 'string') {
    key = Buffer.from(key as string, encoding);
  } else if (isAnyArrayBuffer(key)) {
    // We want the key to be a copy of the original buffer, not a view.
    key = Buffer.from(new Uint8Array(key));
  } else if (isArrayBufferView(key)) {
    // We want the key to be a copy of the original buffer, not a view.
    key = Buffer.from(key as any);
  }

  // Node.js requires that the key data be less than 2 ** 32 - 1,
  // however it enforces the limit silently... returning an empty
  // key as opposed to throwing an error. Silly Node.js.
  // But, it's all good because our runtime limits the size of
  // buffer allocations to a strict maximum of 2,147,483,646 ... way
  // more than necessary... no one actually *needs* a 17,179,869,168
  // bit secret key do they? Good luck to the poor soul who tries.

  return KeyObject.from(cryptoImpl.createSecretKey(key)) as SecretKeyObject;
}

function prepareAsymmetricKey(
  key: CreateAsymmetricKeyOptions | KeyData,
  ctx: KeyContext
): InnerCreateAsymmetricKeyOptions {
  // Safety check... key should not be undefined or null here.
  if ((key as any) == null) {
    throw new ERR_INVALID_ARG_TYPE(
      'key',
      ['ArrayBuffer', 'Buffer', 'TypedArray', 'DataView', 'string', 'object'],
      key
    );
  }

  let normalized: CreateAsymmetricKeyOptions;
  if (
    isStringOrBuffer(key) ||
    isAnyArrayBuffer(key) ||
    isArrayBufferView(key)
  ) {
    normalized = { key, format: 'pem' } as CreateAsymmetricKeyOptions;
  } else {
    normalized = key as CreateAsymmetricKeyOptions;
  }

  const {
    key: data,
    encoding = 'utf8',
    format = 'pem',
    type,
    passphrase,
  } = normalized;

  // The key data must be specified. The value has to be one of either a
  // string, an ArrayBuffer, an ArrayBufferView, or a JWK object.
  if ((data as any) == null || isKeyObject(data) || data instanceof CryptoKey) {
    throw new ERR_INVALID_ARG_TYPE(
      'options.key',
      ['ArrayBuffer', 'Buffer', 'TypedArray', 'DataView', 'string', 'object'],
      data
    );
  }

  if (isStringOrBuffer(data)) {
    // When the key data is a string or buffer, the format must be
    // one of either pem or der.
    validateOneOf(format, 'format', ['pem', 'der']);
    if (type !== undefined) {
      if (ctx == KeyContext.kCreatePrivate) {
        // When the key data is a string or buffer, the type must be
        // one of either pkcs1, pkcs8, or sec1.
        validateOneOf(type, 'type', ['pkcs1', 'pkcs8', 'sec1']);
      } else if (ctx == KeyContext.kCreatePublic) {
        validateOneOf(type, 'type', ['pkcs1', 'spki']);
      }
    }
    return {
      key: getArrayBufferOrView(data, 'key', encoding),
      format,
      type,
      passphrase:
        passphrase != null
          ? getArrayBufferOrView(passphrase, 'passphrase', encoding)
          : undefined,
    };
  }

  // Final type check. The key data at this point has to be an object that
  // we will interpret as a JWK.
  if (typeof data !== 'object') {
    throw new ERR_INVALID_ARG_TYPE(
      'key',
      ['ArrayBuffer', 'Buffer', 'TypedArray', 'DataView', 'string', 'object'],
      key
    );
  }

  // At this point we ignore all remaining options and assume the key is a
  // JSON Web Key.
  return {
    key: data as JsonWebKey,
    format: 'jwk',
    type: undefined,
    passphrase: undefined,
  };
}

export function createPrivateKey(key: string): PrivateKeyObject;
export function createPrivateKey(
  key: ArrayBuffer | ArrayBufferView
): PrivateKeyObject;
export function createPrivateKey(
  key: CreateAsymmetricKeyOptions
): PrivateKeyObject;
export function createPrivateKey(
  key: CreateAsymmetricKeyOptions | KeyData
): PrivateKeyObject {
  const cryptoKey = cryptoImpl.createPrivateKey(
    prepareAsymmetricKey(key, KeyContext.kCreatePrivate)
  );
  return KeyObject.from(cryptoKey) as PrivateKeyObject;
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
  // This API is not implemented yet.
  callback(new ERR_METHOD_NOT_IMPLEMENTED('crypto.generateKeySync'));
}

export function generateKeySync(
  _type: SecretKeyType,
  _options: GenerateKeyOptions
) {
  // This API is not implemented yet.
  throw new ERR_METHOD_NOT_IMPLEMENTED('crypto.generateKeySync');
}

export function generateKeyPair(
  _type: AsymmetricKeyType,
  _options: GenerateKeyPairOptions,
  callback: GenerateKeyPairCallback
) {
  // This API is not implemented yet.
  callback(new ERR_METHOD_NOT_IMPLEMENTED('crypto.generateKeyPair'));
}

export function generateKeyPairSync(
  _type: AsymmetricKeyType,
  _options: GenerateKeyPairOptions
): KeyObjectPair {
  // This API is not implemented yet.
  throw new ERR_METHOD_NOT_IMPLEMENTED('crypto.generateKeyPairSync');
}
