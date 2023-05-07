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

export {
  randomBytes,
  randomFillSync,
  randomFill,
  randomInt,
  randomUUID,
  PrimeNum as primeNum,
  GeneratePrimeOptions as generatePrimeOptions,
  CheckPrimeOptions as checkPrimeOptions,
  generatePrime,
  generatePrimeSync,
  checkPrime,
  checkPrimeSync,
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
//   * [ ] crypto.DiffieHellman
//   * [ ] crypto.DiffieHellmanGroup
//   * [ ] crypto.ECDH
//   * [ ] crypto.Hash
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
//   * [ ] crypto.createDiffieHellman(prime[, primeEncoding][, generator][, generatorEncoding])
//   * [ ] crypto.createDiffieHellman(primeLength[, generator])
//   * [ ] crypto.createDiffieHellmanGroup(name)
//   * [ ] crypto.createECDH(curveName)
//   * [ ] crypto.diffieHellman(options)
//   * [ ] crypto.getDiffieHellman(groupName)
// * Hash
//   * [ ] crypto.createHash(algorithm[, options])
//   * [ ] crypto.createHmac(algorithm, key[, options])
//   * [ ] crypto.getHashes()
// * Keys
//   * [ ] crypto.createPrivateKey(key)
//   * [ ] crypto.createPublicKey(key)
//   * [ ] crypto.createSecretKey(key[, encoding])
//   * [ ] crypto.generateKey(type, options, callback)
//   * [ ] crypto.generateKeyPair(type, options, callback)
//   * [ ] crypto.generateKeyPairSync(type, options)
//   * [ ] crypto.generateKeySync(type, options)
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
//   * [ ] crypto.pbkdf2(password, salt, iterations, keylen, digest, callback)
//   * [ ] crypto.pbkdf2Sync(password, salt, iterations, keylen, digest)
//   * [ ] crypto.scrypt(password, salt, keylen[, options], callback)
//   * [ ] crypto.scryptSync(password, salt, keylen[, options])
// * WebCrypto
//   * [x] crypto.subtle
//   * [x] crypto.webcrypto

