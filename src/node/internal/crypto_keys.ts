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
  type ParamEncoding,
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
  ERR_INCOMPATIBLE_OPTION_PAIR,
  ERR_INVALID_ARG_TYPE,
  ERR_METHOD_NOT_IMPLEMENTED,
  ERR_MISSING_OPTION,
} from 'node-internal:internal_errors';

import {
  validateFunction,
  validateInteger,
  validateObject,
  validateOneOf,
  validateString,
  validateInt32,
  validateUint32,
} from 'node-internal:validators';

import { inspect } from 'node-internal:internal_inspect';
import { randomBytes } from 'node-internal:crypto_random';
const kInspect = inspect.custom;

const kCustomPromisifyArgsSymbol = Symbol.for(
  'nodejs.util.promisify.custom.args'
);

// Key input contexts.
export enum KeyContext {
  kConsumePublic = 'kConsumePublic',
  kConsumePrivate = 'kConsumePrivate',
  kCreatePublic = 'kCreatePublic',
  kCreatePrivate = 'kCreatePrivate',
}

// In Node.js, the definition of KeyObject is a bit complicated because
// KeyObject instances in Node.js can be transferred via postMessage() and
// structuredClone(), etc, allowing instances to be shared across multiple
// worker threads. We do not implement that model so we're essentially
// re-implementing the Node.js API here instead of just taking their code.
// Also in Node.js, CryptoKey is layered on top of KeyObject since KeyObject
// existed first. We're, however, going to layer our KeyObject on top of
// CryptoKey with a few augmentations.

function isStringOrBuffer(val: unknown): val is string | Buffer {
  return (
    typeof val === 'string' || isArrayBufferView(val) || isAnyArrayBuffer(val)
  );
}

function validateExportOptions(
  options: ExportOptions,
  type: KeyObjectType,
  name = 'options'
): asserts options is ExportOptions {
  validateObject(options, name, {});
  // Yes, converting to any is a bit of a cheat, but it allows us to check
  // each option individually without having to do a bunch of type guards.
  const opts = options;
  if (opts.format !== undefined) {
    validateString(opts.format, `${name}.format`);
  } else {
    options.format = 'buffer';
  }
  if (opts.type !== undefined) validateString(opts.type, `${name}.type`);
  if (type === 'private') {
    if (opts.cipher !== undefined) {
      validateString(opts.cipher, `${name}.cipher`);
      if (typeof opts.passphrase === 'string') {
        opts.passphrase = Buffer.from(opts.passphrase, opts.encoding as string);
      }
      if (!isUint8Array(opts.passphrase)) {
        throw new ERR_INVALID_ARG_TYPE(
          `${name}.passphrase`,
          ['string', 'Uint8Array'],
          opts.passphrase
        );
      }
    }
  }
}

export abstract class KeyObject {
  public [kHandle]: CryptoKey;

  public constructor() {
    // KeyObjects cannot be created with new ... use one of the
    // create or generate methods, or use from to get from a
    // CryptoKey.
    throw new Error('Illegal constructor');
  }

  public static from(key: CryptoKey): KeyObject {
    if (!(key instanceof CryptoKey)) {
      throw new ERR_INVALID_ARG_TYPE('key', 'CryptoKey', key);
    }
    switch (key.type) {
      case 'secret':
        return Reflect.construct(
          function (this: SecretKeyObject): void {
            this[kHandle] = key;
          },
          [],
          SecretKeyObject
        ) as KeyObject;
      case 'private':
        return Reflect.construct(
          function (this: PrivateKeyObject): void {
            this[kHandle] = key;
          },
          [],
          PrivateKeyObject
        ) as KeyObject;
      case 'public':
        return Reflect.construct(
          function (this: PublicKeyObject): void {
            this[kHandle] = key;
          },
          [],
          PublicKeyObject
        ) as KeyObject;
    }
  }

  public export(options: ExportOptions = {}): KeyExportResult {
    validateObject(options, 'options', {});

    validateExportOptions(options, this.type);

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

  public equals(otherKeyObject: KeyObject): boolean {
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

  public abstract get type(): KeyObjectType;

  public get [Symbol.toStringTag](): string {
    return 'KeyObject';
  }
}

export function isKeyObject(obj: unknown): obj is KeyObject {
  return obj != null && typeof obj === 'object' && kHandle in obj;
}

export function getKeyObjectHandle(obj: KeyObject): CryptoKey {
  return obj[kHandle];
}

abstract class AsymmetricKeyObject extends KeyObject {
  public get asymmetricKeyDetails(): AsymmetricKeyDetails {
    const detail = cryptoImpl.getAsymmetricKeyDetail(this[kHandle]);
    if (isArrayBuffer(detail.publicExponent)) {
      detail.publicExponent = arrayBufferToUnsignedBigInt(
        detail.publicExponent
      );
    }
    return detail;
  }

  public get asymmetricKeyType(): AsymmetricKeyType {
    return cryptoImpl.getAsymmetricKeyType(this[kHandle]);
  }

  public toCryptoKey(): void {
    // TODO(soon): Implement the toCryptoKey API (added in Node.js 23.0.0)
    throw new ERR_METHOD_NOT_IMPLEMENTED('toCryptoKey');
  }

  public [kInspect](
    depth: number,
    options: {
      depth?: number;
    }
  ): string | this {
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
  public override export(options?: PublicKeyExportOptions): KeyExportResult {
    return super.export(options);
  }

  public get type(): KeyObjectType {
    return 'public';
  }
}

export class PrivateKeyObject extends AsymmetricKeyObject {
  public override export(options?: PrivateKeyExportOptions): KeyExportResult {
    return super.export(options);
  }

  public get type(): KeyObjectType {
    return 'private';
  }
}

export class SecretKeyObject extends KeyObject {
  public get symmetricKeySize(): number {
    return (this[kHandle].algorithm as unknown as string).length | 0;
  }

  public override export(options?: SecretKeyExportOptions): KeyExportResult {
    return super.export(options);
  }

  public get type(): KeyObjectType {
    return 'secret';
  }

  public [kInspect](depth: number, options: { depth?: number }): string | this {
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
): void {
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
    key = Buffer.from(key, encoding);
  } else if (isAnyArrayBuffer(key)) {
    // We want the key to be a copy of the original buffer, not a view.
    key = Buffer.from(new Uint8Array(key));
  } else if (isArrayBufferView(key)) {
    // We want the key to be a copy of the original buffer, not a view.
    key = Buffer.from(key as Buffer);
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

export function prepareAsymmetricKey(
  key: CreateAsymmetricKeyOptions | null | undefined,
  ctx: KeyContext
): InnerCreateAsymmetricKeyOptions {
  // Safety check... key should not be undefined or null here.
  if (key == null) {
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
    normalized = key;
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
  if (data == null || isKeyObject(data) || data instanceof CryptoKey) {
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
    prepareAsymmetricKey(
      key as CreateAsymmetricKeyOptions,
      KeyContext.kCreatePrivate
    )
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
  key: CreateAsymmetricKeyOptions | KeyData | CryptoKey | KeyObject
): PublicKeyObject {
  // Passing a KeyObject or a CryptoKey allows deriving the public key
  // from an existing private key.

  if (isKeyObject(key)) {
    if (key.type !== 'private') {
      throw new ERR_INVALID_ARG_TYPE('key', 'PrivateKeyObject', key);
    }
    return KeyObject.from(
      cryptoImpl.createPublicKey({
        key: key[kHandle],
        // The following are ignored when key is a CryptoKey.
        format: 'pem',
        type: undefined,
        passphrase: undefined,
      })
    ) as PublicKeyObject;
  }

  if (key instanceof CryptoKey) {
    if (key.type !== 'private') {
      throw new ERR_INVALID_ARG_TYPE('key', 'PrivateKeyObject', key);
    }
    return KeyObject.from(
      cryptoImpl.createPublicKey({
        key,
        // The following are ignored when key is a CryptoKey.
        format: 'pem',
        type: undefined,
        passphrase: undefined,
      })
    ) as PublicKeyObject;
  }

  const cryptoKey = cryptoImpl.createPublicKey(
    prepareAsymmetricKey(
      key as CreateAsymmetricKeyOptions,
      KeyContext.kCreatePublic
    )
  );
  return KeyObject.from(cryptoKey) as PublicKeyObject;
}

// ======================================================================================

export type PublicKeyResult = KeyExportResult | PublicKeyObject;
export type PrivateKeyResult = KeyExportResult | PrivateKeyObject;
export type GenerateKeyCallback = (
  err?: unknown,
  key?: SecretKeyObject
) => void;
export type GenerateKeyPairCallback = (
  err?: unknown,
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
): void {
  try {
    // Unlike Node.js, which implements async crypto functions using the
    // libuv thread pool, we don't actually perform async crypto operations.
    // Here we just defer to the sync version of the function and then "fake"
    // async by using queueMicrotask to call the callback.
    const result = generateKeySync(_type, _options);
    queueMicrotask(() => {
      try {
        callback(null, result);
      } catch (err) {
        reportError(err);
      }
    });
  } catch (err) {
    queueMicrotask(() => {
      try {
        callback(err);
      } catch (otherErr) {
        reportError(otherErr);
      }
    });
  }
}

export function generateKeyPair(
  _type: AsymmetricKeyType,
  _options: GenerateKeyPairOptions,
  callback: GenerateKeyPairCallback
): void {
  validateFunction(callback, 'callback');
  try {
    // Unlike Node.js, which implements async crypto functions using the
    // libuv thread pool, we don't actually perform async crypto operations.
    // Here we just defer to the sync version of the function and then "fake"
    // async by using queueMicrotask to call the callback.
    const { publicKey, privateKey } = generateKeyPairSync(_type, _options);
    queueMicrotask(() => {
      try {
        callback(null, publicKey, privateKey);
      } catch (err) {
        reportError(err);
      }
    });
  } catch (err) {
    queueMicrotask(() => {
      try {
        callback(err);
      } catch (otherErr) {
        reportError(otherErr);
      }
    });
  }
}

Object.defineProperty(generateKeyPair, kCustomPromisifyArgsSymbol, {
  value: ['publicKey', 'privateKey'],
  enumerable: false,
});

export function generateKeySync(
  type: SecretKeyType,
  options: GenerateKeyOptions
): SecretKeyObject {
  validateOneOf(type, 'type', ['hmac', 'aes']);
  validateObject(options, 'options');
  const { length } = options;

  switch (type) {
    case 'hmac': {
      // The minimum is 8, and the maximum length is 65,536. If the length is
      // not a multiple of 8, the generated key will be truncated to
      // Math.floor(length / 8).
      // Note that the upper bound of 65536 is intentionally more limited than
      // what Node.js allows. This puts the maximum size limit on generated
      // secret keys to 8192 bytes. We can adjust this up if necessary but
      // it's a good starting point.
      validateInteger(length, 'options.length', 8, 65536);
      const buf = randomBytes(Math.floor(length / 8));
      return createSecretKey(buf);
    }
    case 'aes': {
      // The length must be one of 128, 192, or 256.
      validateOneOf(length, 'options.length', [128, 192, 256]);
      const buf = randomBytes(length / 8);
      return createSecretKey(buf);
    }
  }
}

export function generateKeyPairSync(
  type: AsymmetricKeyType,
  options: GenerateKeyPairOptions = {}
): KeyObjectPair {
  validateOneOf(type, 'type', [
    'rsa',
    'ec',
    'ed25519',
    'x25519',
    'dh',
    // BoringSSL does not support the 448 variants.
    // 'dsa',
    // 'rsa-pss',
    // 'ed448',
    // 'x448',
  ]);

  validateObject(options, 'options');

  const {
    modulusLength, // Used for RSA/DSA. number
    publicExponent = 0x10001, // Used for RSA. number
    // Historically Node.js had "hash" and "mgf1Hash" but these were deprecated.
    // It is still possible to find uses of the old names in the wild.
    // TODO(later): Uncomment the following when rsa-pss generation is supported.
    // hashAlgorithm, // Used for RSA-PSS. string
    // mgf1HashAlgorithm, // Used for RSA-PSS. string
    // hash, // Deprecated, use hashAlgorithm instead. string
    // mgf1Hash, // Deprecated, use mgf1HashAlgorithm instead. string
    // saltLength, // Used for RSA-PSS. number
    namedCurve, // Used for EC. string
    prime, // Used for DH. Buffer/ArrayBufferView/ArrayBuffer
    primeLength, // Used for DH. number
    generator, // Used for DH. number
    // This is fun... Node.js docs say the option is "groupName" while
    // the code appears to check for "group".
    group, // Used for DH (alias for groupName)
    groupName, // Used for DH. string
    paramEncoding = 'named', // For for EC. Value is 'named' or 'explicit'.
    publicKeyEncoding, // value must be an object, same options as export
    privateKeyEncoding, // value must be an object, same options as export
  } = options;

  // TODO(later): The divisorLength option is only used for generating DSA
  // keypairs, which we currently do not support.
  // let { divisorLength } = options;

  if (publicKeyEncoding !== undefined) {
    validateExportOptions(
      publicKeyEncoding as ExportOptions,
      'public',
      'options.publicKeyEncoding'
    );
  }
  if (privateKeyEncoding !== undefined) {
    validateExportOptions(
      privateKeyEncoding as ExportOptions,
      'private',
      'options.privateKeyEncoding'
    );
  }

  const handleKeyEncoding = (
    pair: CryptoKeyPair
  ): {
    publicKey: KeyObject | KeyExportResult;
    privateKey: KeyObject | KeyExportResult;
  } => {
    let publicKey: KeyExportResult | KeyObject = KeyObject.from(pair.publicKey);
    let privateKey: KeyExportResult | KeyObject = KeyObject.from(
      pair.privateKey
    );
    if (publicKeyEncoding !== undefined) {
      publicKey = publicKey.export(publicKeyEncoding as PublicKeyExportOptions);
    }
    if (privateKeyEncoding !== undefined) {
      privateKey = privateKey.export(
        privateKeyEncoding as PrivateKeyExportOptions
      );
    }
    return { publicKey, privateKey };
  };

  // Validation of the specific options depends on the type of key being
  // generated.

  switch (type) {
    case 'rsa': {
      validateUint32(modulusLength, 'options.modulusLength');
      validateUint32(publicExponent, 'options.publicExponent');
      return handleKeyEncoding(
        cryptoImpl.generateRsaKeyPair({
          type,
          modulusLength: modulusLength,
          publicExponent: publicExponent,
        })
      ) as KeyObjectPair;
    }
    // TODO(later): BoringSSL does not support RSA-PSS key generation in the
    // same way as Node.js. Later see if there's an alternative approach.
    // case 'rsa-pss': {
    //   validateUint32(modulusLength, 'options.modulusLength');
    //   validateUint32(publicExponent, 'options.publicExponent');

    //     if (saltLength !== undefined) {
    //       validateInt32(saltLength, 'options.saltLength', 0);
    //     }
    //     if (hashAlgorithm !== undefined) {
    //       validateString(hashAlgorithm, 'options.hashAlgorithm');
    //     }
    //     if (mgf1HashAlgorithm !== undefined) {
    //       validateString(mgf1HashAlgorithm, 'options.mgf1HashAlgorithm');
    //     }
    //     if (hash !== undefined) {
    //       validateString(hash, 'options.hash');
    //       if (hashAlgorithm && hash !== hashAlgorithm) {
    //         throw new ERR_INVALID_ARG_VALUE('options.hash', hash);
    //       }
    //     }
    //     if (mgf1Hash !== undefined) {
    //       validateString(mgf1Hash, 'options.mgf1Hash');
    //       if (mgf1HashAlgorithm && mgf1Hash !== mgf1HashAlgorithm) {
    //         throw new ERR_INVALID_ARG_VALUE('options.mgf1Hash', mgf1Hash);
    //       }
    //     }
    //     return handleKeyEncoding(
    //       cryptoImpl.generateRsaKeyPair({
    //         type,
    //         modulusLength: modulusLength!,
    //         publicExponent: publicExponent!,
    //         saltLength: saltLength!,
    //         hashAlgorithm: hash || hashAlgorithm!,
    //         mgf1HashAlgorithm: mgf1Hash || mgf1HashAlgorithm!,
    //       })
    //     ) as KeyObjectPair;
    //  }
    // TODO(later): BoringSSL does not support DSA key generation in the
    // same way as Node.js. Later see if there's an alternative approach.
    // case 'dsa': {
    //   validateUint32(modulusLength, 'options.modulusLength');
    //   if (divisorLength == null) {
    //     divisorLength = undefined;
    //   } else {
    //     validateInt32(divisorLength, 'options.divisorLength', 0);
    //   }
    //   return handleKeyEncoding(
    //     cryptoImpl.generateDsaKeyPair({
    //       modulusLength: modulusLength!,
    //       divisorLength: divisorLength as number,
    //     })
    //   ) as KeyObjectPair;
    // }
    case 'ec': {
      validateString(namedCurve, 'options.namedCurve');
      validateOneOf(paramEncoding, 'options.paramEncoding', [
        'named',
        'explicit',
      ]);
      return handleKeyEncoding(
        cryptoImpl.generateEcKeyPair({
          namedCurve: namedCurve,
          paramEncoding: paramEncoding as ParamEncoding,
        })
      ) as KeyObjectPair;
    }
    case 'ed25519':
    // Fall-through
    // eslint-disable-next-line no-fallthrough
    case 'x25519': {
      // Nothing to validate...
      return handleKeyEncoding(
        cryptoImpl.generateEdKeyPair({ type })
      ) as KeyObjectPair;
    }
    case 'dh': {
      if (generator != null) {
        validateInt32(generator, 'options.generator', 0);
      }

      if (group != null || groupName != null) {
        if (prime != null) {
          throw new ERR_INCOMPATIBLE_OPTION_PAIR('group', 'prime');
        }
        if (primeLength != null) {
          throw new ERR_INCOMPATIBLE_OPTION_PAIR('group', 'primeLength');
        }
        if (generator != null) {
          throw new ERR_INCOMPATIBLE_OPTION_PAIR('group', 'generator');
        }

        const g = group || groupName;

        validateString(g, 'options.group');

        return handleKeyEncoding(
          cryptoImpl.generateDhKeyPair({
            primeOrGroup: g,
            // TODO(soon): Fix this assertion.
            generator: generator as unknown as number,
          })
        ) as KeyObjectPair;
      }

      if (prime != null) {
        if (primeLength != null) {
          throw new ERR_INCOMPATIBLE_OPTION_PAIR('prime', 'primeLength');
        }

        if (!isArrayBufferView(prime) && !isAnyArrayBuffer(prime)) {
          throw new ERR_INVALID_ARG_TYPE(
            'options.prime',
            ['Buffer', 'TypedArray', 'ArrayBuffer'],
            prime
          );
        }
      } else if (primeLength != null) {
        validateInt32(primeLength, 'options.primeLength', 0);
      } else {
        throw new ERR_MISSING_OPTION(
          'At least one of the group, prime, or primeLength options'
        );
      }

      if (prime) {
        return handleKeyEncoding(
          cryptoImpl.generateDhKeyPair({
            primeOrGroup: prime as BufferSource,
            generator: generator as number,
          })
        ) as KeyObjectPair;
      }

      return handleKeyEncoding(
        cryptoImpl.generateDhKeyPair({
          primeOrGroup: primeLength as number,
          generator: generator as number,
        })
      ) as KeyObjectPair;
    }
  }
}
