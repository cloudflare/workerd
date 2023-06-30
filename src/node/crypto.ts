// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
/* eslint-disable */

// TODO(soon): Remove this once assert is out of experimental
import { default as CompatibilityFlags } from 'workerd:compatibility-flags';
if (!CompatibilityFlags.workerdExperimental) {
  throw new Error('node:crypto is experimental.');
}

import {
  ERR_METHOD_NOT_IMPLEMENTED
} from 'node-internal:internal_errors';

export const getRandomValues = crypto.getRandomValues;
export const subtle = crypto.subtle;
export const timingSafeEqual = (crypto as any).timingSafeEqual;
export const webcrypto = crypto;

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
  Hash,
  HashOptions,
} from 'node-internal:crypto_hash';

import {
  pbkdf2,
  pbkdf2Sync,
  ArrayLike,
} from 'node-internal:crypto_pbkdf2';

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
  // Hash
  createHash,
  Hash,
  HashOptions,
  // Pbkdf2
  pbkdf2,
  pbkdf2Sync,
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
}

export function getHashes() {
  // Hardcoded list of hashes supported in boringssl, node's approach looks pretty clunky. This is
  // expected to change infrequently based of bssl's stability-focused approach.
  return ['md4', 'md5', 'sha1', 'sha224', 'sha256', 'sha384', 'sha512', 'md5-sha1', 'RSA-MD5',
          'RSA-SHA1', 'RSA-SHA224', 'RSA-SHA256', 'RSA-SHA384', 'RSA-SHA512', 'DSA-SHA',
          'DSA-SHA1', 'ecdsa-with-SHA1'];
}

// We do not implement the openssl secure heap.
export function secureHeapUsed() {
  return {
    total: 0,
    used: 0,
    utilization: 0,
    min: 0,
  }
}

// We do not allow users to set the engine used.
export function setEngine(_1 : string, _2?: number) {
  throw new ERR_METHOD_NOT_IMPLEMENTED('setEngine');
}

// We do not allow users to modify the FIPS enablement.
export function setFips(_: boolean) {
  throw new ERR_METHOD_NOT_IMPLEMENTED('setFips');
}

// We always run in FIPS mode.
export const fips = true;
export function getFips() { return fips; }

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
  // Hash
  Hash,
  createHash,
  getHashes,
  // Pbkdf2
  pbkdf2,
  pbkdf2Sync,
  // Misc
  secureHeapUsed,
  setEngine,
  timingSafeEqual,
  // Fips
  getFips,
  setFips,
  get fips() { return getFips(); },
  set fips(_: boolean) { setFips(_); },
  // WebCrypto
  subtle,
  webcrypto,
};

// Classes
//   * [ ] crypto.Certificate
//   * [ ] crypto.Cipher
//   * [ ] crypto.Decipher
//   * [x] crypto.DiffieHellman
//   * [x] crypto.DiffieHellmanGroup
//   * [ ] crypto.ECDH
//   * [x] crypto.Hash
//   * [ ] crypto.Hmac
//   * [ ] crypto.KeyObject
//   * [ ] crypto.Sign
//   * [ ] crypto.Verify
//   * [ ] crypto.X509Certificate
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
//   * [ ] crypto.createHmac(algorithm, key[, options])
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
//   * [ ] crypto.getCiphers()
//   * [ ] crypto.getCurves()
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
//   * [ ] crypto.hkdf(digest, ikm, salt, info, keylen, callback)
//   * [ ] crypto.hkdfSync(digest, ikm, salt, info, keylen)
//   * [x] crypto.pbkdf2(password, salt, iterations, keylen, digest, callback)
//   * [x] crypto.pbkdf2Sync(password, salt, iterations, keylen, digest)
//   * [ ] crypto.scrypt(password, salt, keylen[, options], callback)
//   * [ ] crypto.scryptSync(password, salt, keylen[, options])
// * WebCrypto
//   * [x] crypto.subtle
//   * [x] crypto.webcrypto

