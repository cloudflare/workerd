// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, strictEqual } from 'node:assert';

async function checkExport(key, algorithm, kty, fields, alg) {
  const jwk = await crypto.subtle.exportKey('jwk', key);
  const expected = ['kty', 'key_ops', 'ext', ...fields];
  if (alg !== undefined) expected.push('alg');
  expected.sort();

  deepStrictEqual(Object.keys(jwk).sort(), expected);
  deepStrictEqual(Object.getOwnPropertyNames(jwk).sort(), expected);
  deepStrictEqual(Object.getOwnPropertySymbols(jwk), []);
  strictEqual(jwk.kty, kty);
  strictEqual(jwk.ext, true);
  deepStrictEqual(jwk.key_ops, key.usages);
  strictEqual(jwk.alg, alg);
  for (const field of fields) {
    strictEqual(typeof jwk[field], 'string');
    strictEqual(jwk[field].length > 0, true);
  }
  if (key.type === 'public') {
    for (const field of ['d', 'priv', 'k']) {
      strictEqual(field in jwk, false);
      strictEqual(Object.hasOwn(jwk, field), false);
    }
  }

  const imported = await crypto.subtle.importKey(
    'jwk',
    jwk,
    algorithm,
    true,
    key.usages
  );
  deepStrictEqual(await crypto.subtle.exportKey('jwk', imported), jwk);
  return jwk;
}

export const secretKeys = {
  async test() {
    for (const [algorithm, usages, alg] of [
      [{ name: 'AES-GCM', length: 256 }, ['encrypt', 'decrypt'], 'A256GCM'],
      [{ name: 'HMAC', hash: 'SHA-256' }, ['sign', 'verify'], 'HS256'],
      ['ChaCha20-Poly1305', ['encrypt', 'decrypt'], 'C20P'],
    ]) {
      const key = await crypto.subtle.generateKey(algorithm, true, usages);
      await checkExport(key, algorithm, 'oct', ['k'], alg);
    }
  },
};

export const rsaKeys = {
  async test() {
    const algorithm = {
      name: 'RSA-PSS',
      modulusLength: 2048,
      publicExponent: new Uint8Array([1, 0, 1]),
      hash: 'SHA-256',
    };
    const { publicKey, privateKey } = await crypto.subtle.generateKey(
      algorithm,
      true,
      ['sign', 'verify']
    );
    const publicJwk = await checkExport(
      publicKey,
      algorithm,
      'RSA',
      ['n', 'e'],
      'PS256'
    );
    const privateJwk = await checkExport(
      privateKey,
      algorithm,
      'RSA',
      ['n', 'e', 'd', 'p', 'q', 'dp', 'dq', 'qi'],
      'PS256'
    );
    strictEqual(privateJwk.n, publicJwk.n);
    strictEqual(privateJwk.e, publicJwk.e);
  },
};

export const ecKeys = {
  async test() {
    for (const [name, usages] of [
      ['ECDSA', ['sign', 'verify']],
      ['ECDH', ['deriveBits']],
    ]) {
      const algorithm = { name, namedCurve: 'P-256' };
      const { publicKey, privateKey } = await crypto.subtle.generateKey(
        algorithm,
        true,
        usages
      );
      const publicJwk = await checkExport(publicKey, algorithm, 'EC', [
        'crv',
        'x',
        'y',
      ]);
      const privateJwk = await checkExport(privateKey, algorithm, 'EC', [
        'crv',
        'x',
        'y',
        'd',
      ]);
      strictEqual(publicJwk.crv, 'P-256');
      strictEqual(privateJwk.crv, 'P-256');
      strictEqual(privateJwk.x, publicJwk.x);
      strictEqual(privateJwk.y, publicJwk.y);
    }
  },
};

export const okpKeys = {
  async test() {
    for (const [algorithm, usages, alg] of [
      ['Ed25519', ['sign', 'verify'], 'EdDSA'],
      ['X25519', ['deriveBits'], undefined],
    ]) {
      const { publicKey, privateKey } = await crypto.subtle.generateKey(
        algorithm,
        true,
        usages
      );
      const publicJwk = await checkExport(
        publicKey,
        algorithm,
        'OKP',
        ['crv', 'x'],
        alg
      );
      const privateJwk = await checkExport(
        privateKey,
        algorithm,
        'OKP',
        ['crv', 'x', 'd'],
        alg
      );
      strictEqual(publicJwk.crv, algorithm);
      strictEqual(privateJwk.crv, algorithm);
      strictEqual(privateJwk.x, publicJwk.x);
    }
  },
};

export const akpKeys = {
  async test() {
    for (const [algorithm, usages] of [
      ['ML-KEM-768', ['encapsulateBits', 'decapsulateBits']],
      ['ML-DSA-44', ['sign', 'verify']],
      ['MLKEM768-X25519', ['encapsulateBits', 'decapsulateBits']],
    ]) {
      const { publicKey, privateKey } = await crypto.subtle.generateKey(
        algorithm,
        true,
        usages
      );
      const publicJwk = await checkExport(
        publicKey,
        algorithm,
        'AKP',
        ['pub'],
        algorithm
      );
      const privateJwk = await checkExport(
        privateKey,
        algorithm,
        'AKP',
        ['pub', 'priv'],
        algorithm
      );
      strictEqual(privateJwk.pub, publicJwk.pub);
    }
  },
};
