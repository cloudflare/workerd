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
} from 'node-internal:crypto_dh';

import {
  randomBytes,
  randomFillSync,
  randomFill,
  randomInt,
  randomUUID,
  PrimeNum,
  GeneratePrimeOptions,
  CheckPrimeOptions,
  generatePrime,
  generatePrimeSync,
  checkPrime,
  checkPrimeSync,
} from 'node-internal:crypto_random';

import {
  createHash,
  createHmac,
  Hash,
  HashOptions,
  Hmac,
} from 'node-internal:crypto_hash';

import { hkdf, hkdfSync } from 'node-internal:crypto_hkdf';

import { pbkdf2, pbkdf2Sync, ArrayLike } from 'node-internal:crypto_pbkdf2';

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
  // Random
  randomBytes,
  randomFillSync,
  randomFill,
  randomInt,
  randomUUID,
  // Primes
  PrimeNum as primeNum,
  GeneratePrimeOptions as generatePrimeOptions,
  CheckPrimeOptions as checkPrimeOptions,
  generatePrime,
  generatePrimeSync,
  checkPrime,
  checkPrimeSync,
  // Hash and Hmac
  createHash,
  createHmac,
  Hash,
  HashOptions,
  Hmac,
  // Hkdf
  hkdf,
  hkdfSync,
  // Pbkdf2
  pbkdf2,
  pbkdf2Sync,
  // Scrypt
  scrypt,
  scryptSync,
  ArrayLike as arrayLike,
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
};

export function getCiphers(): string[] {
  // prettier-ignore
  return ["aes-128-cbc", "aes-192-cbc", "aes-256-cbc", "aes-128-ctr", "aes-192-ctr", "aes-256-ctr",
  "aes-128-ecb", "aes-192-ecb", "aes-256-ecb", "aes-128-gcm", "aes-192-gcm", "aes-256-gcm",
  "aes-128-ofb", "aes-192-ofb", "aes-256-ofb", "des-ecb", "des-ede", "des-ede-cbc", "rc2-cbc"];
}

export function getCurves(): string[] {
  // Hardcoded list of supported curves. Note that prime256v1 is equivalent to secp256r1, we follow
  // OpenSSL's and bssl's nomenclature here.

  // prettier-ignore
  return ['secp224r1', 'prime256v1', 'secp384r1', 'secp521r1'];
}

export function getHashes(): string[] {
  // Hardcoded list of hashes supported in boringssl, node's approach looks pretty clunky. This is
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

export default {
  // DH
  DiffieHellman,
  DiffieHellmanGroup,
  createDiffieHellman,
  createDiffieHellmanGroup,
  getDiffieHellman,
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
};

// Classes
//   * [x] crypto.Certificate
//   * [ ] crypto.Cipher
//   * [ ] crypto.Decipher
//   * [x] crypto.DiffieHellman
//   * [x] crypto.DiffieHellmanGroup
//   * [ ] crypto.ECDH
//   * [x] crypto.Hash
//   * [x] crypto.Hmac
//   * [ ] crypto.KeyObject
//   * [ ] crypto.Sign
//   * [ ] crypto.Verify
//   * [x] crypto.X509Certificate
//   * [ ] crypto.constants
//   * [ ] crypto.DEFAULT_ENCODING
// * Primes
//   * [x] crypto.checkPrime(candidate[, options], callback)
//   * [x] crypto.checkPrimeSync(candidate[, options])
//   * [x] crypto.generatePrime(size[, options[, callback]])
//   * [x] crypto.generatePrimeSync(size[, options])
// * Ciphers
//   * [ ] crypto.createCipher(algorithm, password[, options])
//   * [ ] crypto.createCipheriv(algorithm, key, iv[, options])
//   * [ ] crypto.createDecipher(algorithm, password[, options])
//   * [ ] crypto.createDecipheriv(algorithm, key, iv[, options])
//   * [ ] crypto.privateDecrypt(privateKey, buffer)
//   * [ ] crypto.privateEncrypt(privateKey, buffer)
//   * [ ] crypto.publicDecrypt(key, buffer)
//   * [ ] crypto.publicEncrypt(key, buffer)
// * DiffieHellman
//   * [x] crypto.createDiffieHellman(prime[, primeEncoding][, generator][, generatorEncoding])
//   * [x] crypto.createDiffieHellman(primeLength[, generator])
//   * [x] crypto.createDiffieHellmanGroup(name)
//   * [ ] crypto.createECDH(curveName)
//   * [ ] crypto.diffieHellman(options)
//   * [x] crypto.getDiffieHellman(groupName)
// * Hash
//   * [x] crypto.createHash(algorithm[, options])
//   * [x] crypto.createHmac(algorithm, key[, options])
//   * [x] crypto.getHashes()
// * Keys
//   * [ ] crypto.createPrivateKey(key)
//   * [ ] crypto.createPublicKey(key)
//   * [x] crypto.createSecretKey(key[, encoding])
//   * [x] crypto.generateKey(type, options, callback)
//   * [x] crypto.generateKeyPair(type, options, callback)
//   * [x] crypto.generateKeyPairSync(type, options)
//   * [x] crypto.generateKeySync(type, options)
// * Sign/Verify
//   * [ ] crypto.createSign(algorithm[, options])
//   * [ ] crypto.createVerify(algorithm[, options])
//   * [ ] crypto.sign(algorithm, data, key[, callback])
//   * [ ] crypto.verify(algorithm, data, key, signature[, callback])
// * Misc
//   * [ ] crypto.getCipherInfo(nameOrNid[, options])
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
//   * [.] crypto.hkdf(digest, ikm, salt, info, keylen, callback) (* still needs KeyObject support)
//   * [.] crypto.hkdfSync(digest, ikm, salt, info, keylen) (* still needs KeyObject support)
//   * [x] crypto.pbkdf2(password, salt, iterations, keylen, digest, callback)
//   * [x] crypto.pbkdf2Sync(password, salt, iterations, keylen, digest)
//   * [x] crypto.scrypt(password, salt, keylen[, options], callback)
//   * [x] crypto.scryptSync(password, salt, keylen[, options])
// * WebCrypto
//   * [x] crypto.subtle
//   * [x] crypto.webcrypto
