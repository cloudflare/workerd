// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';

export const getRandomValues = crypto.getRandomValues.bind(crypto);
export const subtle = crypto.subtle;
export const webcrypto = crypto;

export function timingSafeEqual(
  a: NodeJS.ArrayBufferView,
  b: NodeJS.ArrayBufferView
): boolean {
  return (subtle as any).timingSafeEqual(a, b); // eslint-disable-line
}

import {
  DiffieHellman,
  DiffieHellmanGroup,
  createDiffieHellman,
  createDiffieHellmanGroup,
  getDiffieHellman,
  diffieHellman,
  createECDH,
  ECDH,
} from 'node-internal:crypto_dh';

import {
  randomBytes,
  randomFillSync,
  randomFill,
  randomInt,
  randomUUID,
  type PrimeNum,
  type GeneratePrimeOptions,
  type CheckPrimeOptions,
  generatePrime,
  generatePrimeSync,
  checkPrime,
  checkPrimeSync,
} from 'node-internal:crypto_random';

import {
  createHash,
  createHmac,
  Hash,
  type HashOptions,
  Hmac,
  hash,
} from 'node-internal:crypto_hash';

import {
  createSign,
  createVerify,
  sign,
  verify,
  Sign,
  Verify,
} from 'node-internal:crypto_sign';

import {
  Cipheriv,
  Decipheriv,
  createCipheriv,
  createDecipheriv,
  publicDecrypt,
  publicEncrypt,
  privateDecrypt,
  privateEncrypt,
  getCipherInfo,
  getCiphers,
} from 'node-internal:crypto_cipher';

import { hkdf, hkdfSync } from 'node-internal:crypto_hkdf';

import {
  pbkdf2,
  pbkdf2Sync,
  type ArrayLike,
} from 'node-internal:crypto_pbkdf2';

import { scrypt, scryptSync } from 'node-internal:crypto_scrypt';

import {
  KeyObject,
  PublicKeyObject,
  PrivateKeyObject,
  SecretKeyObject,
  generateKey,
  generateKeyPair,
  generateKeyPairSync,
  generateKeySync,
  createPrivateKey,
  createPublicKey,
  createSecretKey,
} from 'node-internal:crypto_keys';

import { Certificate } from 'node-internal:crypto_spkac';

import { X509Certificate } from 'node-internal:crypto_x509';

export {
  // DH
  DiffieHellman,
  DiffieHellmanGroup,
  createDiffieHellman,
  createDiffieHellmanGroup,
  getDiffieHellman,
  diffieHellman,
  ECDH,
  createECDH,
  // Random
  randomBytes,
  randomFillSync,
  randomFill,
  randomInt,
  randomUUID,
  // Primes
  type PrimeNum as primeNum,
  type GeneratePrimeOptions as generatePrimeOptions,
  type CheckPrimeOptions as checkPrimeOptions,
  generatePrime,
  generatePrimeSync,
  checkPrime,
  checkPrimeSync,
  // Hash and Hmac
  createHash,
  createHmac,
  Hash,
  type HashOptions,
  Hmac,
  hash,
  // Hkdf
  hkdf,
  hkdfSync,
  // Pbkdf2
  pbkdf2,
  pbkdf2Sync,
  // Scrypt
  scrypt,
  scryptSync,
  type ArrayLike as arrayLike,
  // Keys
  KeyObject,
  PublicKeyObject,
  PrivateKeyObject,
  SecretKeyObject,
  generateKey,
  generateKeyPair,
  generateKeyPairSync,
  generateKeySync,
  createPrivateKey,
  createPublicKey,
  createSecretKey,
  // Spkac
  Certificate,
  // X509
  X509Certificate,
  // Sign/Verify
  createSign,
  createVerify,
  sign,
  verify,
  Sign,
  Verify,
  // Cipher/Decipher
  Cipheriv,
  Decipheriv,
  createCipheriv,
  createDecipheriv,
  publicDecrypt,
  publicEncrypt,
  privateDecrypt,
  privateEncrypt,
  getCipherInfo,
  getCiphers,
};

export function getCurves(): string[] {
  // Hardcoded list of supported curves. Note that prime256v1 is equivalent to secp256r1, we follow
  // OpenSSL's and bssl's nomenclature here.

  // prettier-ignore
  return ['secp224r1', 'prime256v1', 'secp384r1', 'secp521r1'];
}

export function getHashes(): string[] {
  // Hardcoded list of hashes supported in BoringSSL, node's approach looks pretty clunky. This is
  // expected to change infrequently based of bssl's stability-focused approach.

  // prettier-ignore
  return ['md4', 'md5', 'sha1', 'sha224', 'sha256', 'sha384', 'sha512', 'md5-sha1', 'RSA-MD5',
          'RSA-SHA1', 'RSA-SHA224', 'RSA-SHA256', 'RSA-SHA384', 'RSA-SHA512', 'DSA-SHA',
          'DSA-SHA1', 'ecdsa-with-SHA1'];
}

// We do not implement the openssl secure heap.
export function secureHeapUsed(): Record<string, unknown> {
  return {
    total: 0,
    used: 0,
    utilization: 0,
    min: 0,
  };
}

// We do not allow users to set the engine used.
export function setEngine(_1: string, _2?: number): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('setEngine');
}

// We do not allow users to modify the FIPS enablement.
export function setFips(_: boolean): void {
  throw new ERR_METHOD_NOT_IMPLEMENTED('setFips');
}

// We always run in FIPS mode.
export const fips = true;
export function getFips(): boolean {
  return fips;
}

export const constants: Record<string, number | string> = Object.create(
  null
) as Record<string, number | string>;
Object.defineProperties(constants, {
  DH_CHECK_P_NOT_SAFE_PRIME: {
    value: 2,
    configurable: false,
    writable: false,
  },
  DH_CHECK_P_NOT_PRIME: {
    value: 1,
    configurable: false,
    writable: false,
  },
  DH_UNABLE_TO_CHECK_GENERATOR: {
    value: 4,
    configurable: false,
    writable: false,
  },
  DH_NOT_SUITABLE_GENERATOR: {
    value: 8,
    configurable: false,
    writable: false,
  },
  RSA_PKCS1_PADDING: {
    value: 1,
    configurable: false,
    writable: false,
  },
  RSA_NO_PADDING: {
    value: 3,
    configurable: false,
    writable: false,
  },
  RSA_PKCS1_OAEP_PADDING: {
    value: 4,
    configurable: false,
    writable: false,
  },
  RSA_X931_PADDING: {
    value: 5,
    configurable: false,
    writable: false,
  },
  RSA_PKCS1_PSS_PADDING: {
    value: 6,
    configurable: false,
    writable: false,
  },
  RSA_PSS_SALTLEN_DIGEST: {
    value: -1,
    configurable: false,
    writable: false,
  },
  RSA_PSS_SALTLEN_MAX_SIGN: {
    value: -2,
    configurable: false,
    writable: false,
  },
  RSA_PSS_SALTLEN_AUTO: {
    value: -2,
    configurable: false,
    writable: false,
  },
  POINT_CONVERSION_COMPRESSED: {
    value: 2,
    configurable: false,
    writable: false,
  },
  POINT_CONVERSION_UNCOMPRESSED: {
    value: 4,
    configurable: false,
    writable: false,
  },
  POINT_CONVERSION_HYBRID: {
    value: 6,
    configurable: false,
    writable: false,
  },

  // The following constants aren't actually used by anything in workers and
  // are provided solely for nomimal compatibility with Node.js.

  // This one is particularly silly to define since we don't actually
  // use openssl but the constant exists in Node.js so we'll define it
  // also. However, we set the value to 0 instead of an actual openssl
  // version number to hopefully avoid confusion ... we don't want code
  // out there inspecting this and assuming openssl is present because
  // we hard coded it to a real openssl version number.
  OPENSSL_VERSION_NUMBER: {
    value: 0,
    configurable: false,
    writable: false,
  },
  SSL_OP_ALL: {
    value: 2147485776,
    configurable: false,
    writable: false,
  },
  SSL_OP_ALLOW_NO_DHE_KEX: {
    value: 1024,
    configurable: false,
    writable: false,
  },
  SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION: {
    value: 262144,
    configurable: false,
    writable: false,
  },
  SSL_OP_CIPHER_SERVER_PREFERENCE: {
    value: 4194304,
    configurable: false,
    writable: false,
  },
  SSL_OP_CISCO_ANYCONNECT: {
    value: 32768,
    configurable: false,
    writable: false,
  },
  SSL_OP_COOKIE_EXCHANGE: {
    value: 8192,
    configurable: false,
    writable: false,
  },
  SSL_OP_CRYPTOPRO_TLSEXT_BUG: {
    value: 2147483648,
    configurable: false,
    writable: false,
  },
  SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS: {
    value: 2048,
    configurable: false,
    writable: false,
  },
  SSL_OP_LEGACY_SERVER_CONNECT: {
    value: 4,
    configurable: false,
    writable: false,
  },
  SSL_OP_NO_COMPRESSION: {
    value: 131072,
    configurable: false,
    writable: false,
  },
  SSL_OP_NO_ENCRYPT_THEN_MAC: {
    value: 524288,
    configurable: false,
    writable: false,
  },
  SSL_OP_NO_QUERY_MTU: {
    value: 4096,
    configurable: false,
    writable: false,
  },
  SSL_OP_NO_RENEGOTIATION: {
    value: 1073741824,
    configurable: false,
    writable: false,
  },
  SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION: {
    value: 65536,
    configurable: false,
    writable: false,
  },
  SSL_OP_NO_SSLv2: {
    value: 0,
    configurable: false,
    writable: false,
  },
  SSL_OP_NO_SSLv3: {
    value: 33554432,
    configurable: false,
    writable: false,
  },
  SSL_OP_NO_TICKET: {
    value: 16384,
    configurable: false,
    writable: false,
  },
  SSL_OP_NO_TLSv1: {
    value: 67108864,
    configurable: false,
    writable: false,
  },
  SSL_OP_NO_TLSv1_1: {
    value: 268435456,
    configurable: false,
    writable: false,
  },
  SSL_OP_NO_TLSv1_2: {
    value: 134217728,
    configurable: false,
    writable: false,
  },
  SSL_OP_NO_TLSv1_3: {
    value: 536870912,
    configurable: false,
    writable: false,
  },
  SSL_OP_PRIORITIZE_CHACHA: {
    value: 2097152,
    configurable: false,
    writable: false,
  },
  SSL_OP_TLS_ROLLBACK_BUG: {
    value: 8388608,
    configurable: false,
    writable: false,
  },
  ENGINE_METHOD_RSA: {
    value: 1,
    configurable: false,
    writable: false,
  },
  ENGINE_METHOD_DSA: {
    value: 2,
    configurable: false,
    writable: false,
  },
  ENGINE_METHOD_DH: {
    value: 4,
    configurable: false,
    writable: false,
  },
  ENGINE_METHOD_RAND: {
    value: 8,
    configurable: false,
    writable: false,
  },
  ENGINE_METHOD_EC: {
    value: 2048,
    configurable: false,
    writable: false,
  },
  ENGINE_METHOD_CIPHERS: {
    value: 64,
    configurable: false,
    writable: false,
  },
  ENGINE_METHOD_DIGESTS: {
    value: 128,
    configurable: false,
    writable: false,
  },
  ENGINE_METHOD_PKEY_METHS: {
    value: 512,
    configurable: false,
    writable: false,
  },
  ENGINE_METHOD_PKEY_ASN1_METHS: {
    value: 1024,
    configurable: false,
    writable: false,
  },
  ENGINE_METHOD_ALL: {
    value: 65535,
    configurable: false,
    writable: false,
  },
  ENGINE_METHOD_NONE: {
    value: 0,
    configurable: false,
    writable: false,
  },
  // The default coreCipherList in Node.js is configurable at build time.
  // It is used as a configuration option in TLS client and server connections.
  // We do not actually use this option in our implementation of TLS, however
  // since we do not actually handle the TLS protocol directly in the runtime.
  // There is no need for this value to match the defaultCoreCipherList in the
  // official Node.js binary.
  defaultCoreCipherList: {
    value: '',
    configurable: false,
    writable: false,
  },
  TLS1_VERSION: {
    value: 769,
    configurable: false,
    writable: false,
  },
  TLS1_1_VERSION: {
    value: 770,
    configurable: false,
    writable: false,
  },
  TLS1_2_VERSION: {
    value: 771,
    configurable: false,
    writable: false,
  },
  TLS1_3_VERSION: {
    value: 772,
    configurable: false,
    writable: false,
  },
  // This last one is silly. It's defined on the crypto.constants object
  // in Node.js but is not actually a constant. We also don't actually
  // use it anywhere ourselves. Since we don't actually have a default
  // cipher list in our implementation, we just set it to an empty string
  // initially.
  defaultCipherList: {
    value: '',
    configurable: true,
    writable: true,
  },
});

// Deprecated but required for backwards compatibility.
export const pseudoRandomBytes = randomBytes;

export const CryptoKey = globalThis.CryptoKey;

export let createCipher: (() => void) | undefined = undefined;
export let createDecipher: (() => void) | undefined = undefined;
export let Cipher: (() => void) | undefined = undefined;
export let Decipher: (() => void) | undefined = undefined;

if (!Cloudflare.compatibilityFlags.remove_nodejs_compat_eol_v22) {
  createCipher = (): void => {
    throw new ERR_METHOD_NOT_IMPLEMENTED('createCipher');
  };
  createDecipher = (): void => {
    throw new ERR_METHOD_NOT_IMPLEMENTED('createDecipher');
  };
  Cipher = (): void => {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Cipher');
  };
  Decipher = (): void => {
    throw new ERR_METHOD_NOT_IMPLEMENTED('Decipher');
  };
}
export default {
  constants,
  // DH
  DiffieHellman,
  DiffieHellmanGroup,
  createDiffieHellman,
  createDiffieHellmanGroup,
  getDiffieHellman,
  ECDH,
  createECDH,
  // Keys,
  KeyObject,
  PublicKeyObject,
  PrivateKeyObject,
  SecretKeyObject,
  generateKey,
  generateKeyPair,
  generateKeyPairSync,
  generateKeySync,
  createPrivateKey,
  createPublicKey,
  createSecretKey,
  // Random
  getRandomValues,
  pseudoRandomBytes,
  randomBytes,
  randomFillSync,
  randomFill,
  randomInt,
  randomUUID,
  generatePrime,
  generatePrimeSync,
  checkPrime,
  checkPrimeSync,
  // Hash and Hmac
  Hash,
  Hmac,
  createHash,
  createHmac,
  getHashes,
  hash,
  // Hkdf
  hkdf,
  hkdfSync,
  // Pbkdf2
  pbkdf2,
  pbkdf2Sync,
  // Scrypt
  scrypt,
  scryptSync,
  // Misc
  getCiphers,
  getCurves,
  secureHeapUsed,
  setEngine,
  timingSafeEqual,
  // Fips
  getFips,
  setFips,
  get fips(): boolean {
    return getFips();
  },
  set fips(_: boolean) {
    setFips(_);
  },
  // WebCrypto
  subtle,
  webcrypto,
  // Spkac
  Certificate,
  // X509
  X509Certificate,
  // Sign/Verify
  createSign,
  createVerify,
  sign,
  verify,
  Sign,
  Verify,
  // Cipher/Decipher
  Cipheriv,
  Decipheriv,
  createCipheriv,
  createDecipheriv,
  publicDecrypt,
  publicEncrypt,
  privateDecrypt,
  privateEncrypt,
  getCipherInfo,
  CryptoKey,

  // EOL
  createCipher,
  createDecipher,
  Cipher,
  Decipher,
};

// Classes
//   * [x] crypto.Certificate
//   * [x] crypto.Cipher
//   * [x] crypto.Decipher
//   * [x] crypto.DiffieHellman
//   * [x] crypto.DiffieHellmanGroup
//   * [x] crypto.ECDH
//   * [x] crypto.Hash
//   * [x] crypto.Hmac
//   * [x] crypto.KeyObject
//   * [x] crypto.Sign
//   * [x] crypto.Verify
//   * [x] crypto.X509Certificate
//   * [x] crypto.constants
//   * [ ] crypto.DEFAULT_ENCODING
// * Primes
//   * [x] crypto.checkPrime(candidate[, options], callback)
//   * [x] crypto.checkPrimeSync(candidate[, options])
//   * [x] crypto.generatePrime(size[, options[, callback]])
//   * [x] crypto.generatePrimeSync(size[, options])
// * Ciphers
//   * [x] crypto.createCipher(algorithm, password[, options])
//   * [x] crypto.createCipheriv(algorithm, key, iv[, options])
//   * [x] crypto.createDecipher(algorithm, password[, options])
//   * [x] crypto.createDecipheriv(algorithm, key, iv[, options])
//   * [x] crypto.privateDecrypt(privateKey, buffer)
//   * [x] crypto.privateEncrypt(privateKey, buffer)
//   * [x] crypto.publicDecrypt(key, buffer)
//   * [x] crypto.publicEncrypt(key, buffer)
//   * [x] crypto.Decipher
//   * [x] crypto.Cipher
// * DiffieHellman
//   * [x] crypto.createDiffieHellman(prime[, primeEncoding][, generator][, generatorEncoding])
//   * [x] crypto.createDiffieHellman(primeLength[, generator])
//   * [x] crypto.createDiffieHellmanGroup(name)
//   * [x] crypto.createECDH(curveName)
//   * [x] crypto.diffieHellman(options)
//   * [x] crypto.getDiffieHellman(groupName)
// * Hash
//   * [x] crypto.createHash(algorithm[, options])
//   * [x] crypto.createHmac(algorithm, key[, options])
//   * [x] crypto.getHashes()
//   * [x] crypto.hash()
// * Keys, not implemented yet. Calling the following APIs will throw a ERR_METHOD_NOT_IMPLEMENTED
//   * [x] crypto.createPrivateKey(key)
//   * [x] crypto.createPublicKey(key)
//   * [x] crypto.createSecretKey(key[, encoding])
//   * [x] crypto.generateKey(type, options, callback)
//   * [x] crypto.generateKeyPair(type, options, callback)
//   * [x] crypto.generateKeyPairSync(type, options)
//   * [x] crypto.generateKeySync(type, options)
// * Sign/Verify
//   * [x] crypto.createSign(algorithm[, options])
//   * [x] crypto.createVerify(algorithm[, options])
//   * [x] crypto.sign(algorithm, data, key[, callback])
//   * [x] crypto.verify(algorithm, data, key, signature[, callback])
// * Misc
//   * [x] crypto.getCipherInfo(nameOrNid[, options])
//   * [x] crypto.getCiphers()
//   * [x] crypto.getCurves()
//   * [x] crypto.secureHeapUsed()
//   * [x] crypto.setEngine(engine[, flags])
//   * [x] crypto.timingSafeEqual(a, b)
// * Fips
//   * [x] crypto.getFips()
//   * [x] crypto.fips
//   * [x] crypto.setFips(bool)
// * Random
//   * [x] crypto.getRandomValues(typedArray)
//   * [x] crypto.randomBytes(size[, callback])
//   * [x] crypto.randomFillSync(buffer[, offset][, size])
//   * [x] crypto.randomFill(buffer[, offset][, size], callback)
//   * [x] crypto.randomInt([min, ]max[, callback])
//   * [x] crypto.randomUUID([options])
// * Key Derivation
//   * [x] crypto.hkdf(digest, ikm, salt, info, keylen, callback)
//   * [x] crypto.hkdfSync(digest, ikm, salt, info, keylen)
//   * [x] crypto.pbkdf2(password, salt, iterations, keylen, digest, callback)
//   * [x] crypto.pbkdf2Sync(password, salt, iterations, keylen, digest)
//   * [x] crypto.scrypt(password, salt, keylen[, options], callback)
//   * [x] crypto.scryptSync(password, salt, keylen[, options])
// * WebCrypto
//   * [x] crypto.subtle
//   * [x] crypto.webcrypto
//   * [x] crypto.CryptoKey
