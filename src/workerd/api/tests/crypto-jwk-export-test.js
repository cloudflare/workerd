// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, strictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';
import {
  KeyObject,
  createPrivateKey,
  createPublicKey,
  createSecretKey,
  generateKeyPairSync,
} from 'node:crypto';

function checkNodeExport(key, kty, fields, metadata = {}) {
  const jwk = key.export({ format: 'jwk' });
  const expected = ['kty', ...fields, ...Object.keys(metadata)];
  expected.sort();

  deepStrictEqual(Object.keys(jwk).sort(), expected);
  deepStrictEqual(Object.getOwnPropertyNames(jwk).sort(), expected);
  deepStrictEqual(Object.getOwnPropertySymbols(jwk), []);
  strictEqual(jwk.kty, kty);
  for (const field of ['alg', 'key_ops', 'ext']) {
    const present = Object.hasOwn(metadata, field);
    strictEqual(field in jwk, present);
    strictEqual(Object.hasOwn(jwk, field), present);
    deepStrictEqual(jwk[field], metadata[field]);
  }
  deepStrictEqual(JSON.parse(JSON.stringify(jwk)), jwk);
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
  return jwk;
}

async function checkExport(env, key, algorithm, kty, fields, alg) {
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

  const metadata = {};
  if (!env.NODE_JWK_EXPORT) {
    metadata.key_ops = key.usages;
    metadata.ext = true;
  }
  if ((kty === 'AKP' || !env.NODE_JWK_EXPORT) && alg !== undefined) {
    metadata.alg = alg;
  }
  const nodeJwk = checkNodeExport(KeyObject.from(key), kty, fields, metadata);
  for (const field of ['kty', ...fields]) {
    strictEqual(nodeJwk[field], jwk[field]);
  }
  if (kty !== 'AKP') {
    const nodeKey =
      kty === 'oct'
        ? createSecretKey(Buffer.from(jwk.k, 'base64url'))
        : key.type === 'private'
          ? createPrivateKey({ key: jwk, format: 'jwk' })
          : createPublicKey({ key: jwk, format: 'jwk' });
    const nativeMetadata = env.NODE_JWK_EXPORT
      ? {}
      : kty === 'oct'
        ? { ext: true }
        : kty === 'OKP' && jwk.crv === 'Ed25519'
          ? { alg: 'EdDSA' }
          : {};
    const nativeJwk = checkNodeExport(nodeKey, kty, fields, nativeMetadata);
    for (const field of ['kty', ...fields]) {
      strictEqual(nativeJwk[field], jwk[field]);
    }
  }
  return jwk;
}

export const nodeGeneratedKeys = {
  test(_, env) {
    for (const [type, options, kty, fields, privateFields] of [
      [
        'rsa',
        { modulusLength: 2048 },
        'RSA',
        ['n', 'e'],
        ['d', 'p', 'q', 'dp', 'dq', 'qi'],
      ],
      ['ec', { namedCurve: 'prime256v1' }, 'EC', ['crv', 'x', 'y'], ['d']],
      ['ed25519', {}, 'OKP', ['crv', 'x'], ['d']],
      ['x25519', {}, 'OKP', ['crv', 'x'], ['d']],
    ]) {
      const { publicKey, privateKey } = generateKeyPairSync(type, options);
      const metadata =
        !env.NODE_JWK_EXPORT && type === 'ed25519' ? { alg: 'EdDSA' } : {};
      const publicJwk = checkNodeExport(publicKey, kty, fields, metadata);
      const privateJwk = checkNodeExport(
        privateKey,
        kty,
        [...fields, ...privateFields],
        metadata
      );
      for (const field of fields) {
        strictEqual(privateJwk[field], publicJwk[field]);
      }
      deepStrictEqual(
        checkNodeExport(
          createPublicKey({ key: publicJwk, format: 'jwk' }),
          kty,
          fields,
          metadata
        ),
        publicJwk
      );
      deepStrictEqual(
        checkNodeExport(
          createPrivateKey({ key: privateJwk, format: 'jwk' }),
          kty,
          [...fields, ...privateFields],
          metadata
        ),
        privateJwk
      );
    }
  },
};

export const secretKeys = {
  async test(_, env) {
    for (const [algorithm, usages, alg] of [
      [{ name: 'AES-GCM', length: 256 }, ['encrypt', 'decrypt'], 'A256GCM'],
      [{ name: 'HMAC', hash: 'SHA-256' }, ['sign', 'verify'], 'HS256'],
      ['ChaCha20-Poly1305', ['encrypt', 'decrypt'], 'C20P'],
    ]) {
      const key = await crypto.subtle.generateKey(algorithm, true, usages);
      await checkExport(env, key, algorithm, 'oct', ['k'], alg);
    }
  },
};

export const rsaKeys = {
  async test(_, env) {
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
      env,
      publicKey,
      algorithm,
      'RSA',
      ['n', 'e'],
      'PS256'
    );
    const privateJwk = await checkExport(
      env,
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
  async test(_, env) {
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
      const publicJwk = await checkExport(env, publicKey, algorithm, 'EC', [
        'crv',
        'x',
        'y',
      ]);
      const privateJwk = await checkExport(env, privateKey, algorithm, 'EC', [
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
  async test(_, env) {
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
        env,
        publicKey,
        algorithm,
        'OKP',
        ['crv', 'x'],
        alg
      );
      const privateJwk = await checkExport(
        env,
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
  async test(_, env) {
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
        env,
        publicKey,
        algorithm,
        'AKP',
        ['pub'],
        algorithm
      );
      const privateJwk = await checkExport(
        env,
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
