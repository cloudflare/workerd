// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, ok, rejects, strictEqual } from 'node:assert';

// secp256k1 generator G (the public key for private key d = 1), defined in SEC 2 §2.4.1.
const GENERATOR_PUBKEY_COMPRESSED = new Uint8Array([
  0x02, 0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac, 0x55, 0xa0, 0x62, 0x95,
  0xce, 0x87, 0x0b, 0x07, 0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9, 0x59,
  0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98,
]);
const GENERATOR_PUBKEY_UNCOMPRESSED = new Uint8Array([
  0x04, 0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac, 0x55, 0xa0, 0x62, 0x95,
  0xce, 0x87, 0x0b, 0x07, 0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9, 0x59,
  0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98, 0x48, 0x3a, 0xda, 0x77, 0x26, 0xa3,
  0xc4, 0x65, 0x5d, 0xa4, 0xfb, 0xfc, 0x0e, 0x11, 0x08, 0xa8, 0xfd, 0x17, 0xb4,
  0x48, 0xa6, 0x85, 0x54, 0x19, 0x9c, 0x47, 0xd0, 0x8f, 0xfb, 0x10, 0xd4, 0xb8,
]);

const ECDSA = { name: 'ECDSA', namedCurve: 'secp256k1' };
const ECDSA_SHA256 = { name: 'ECDSA', hash: 'SHA-256' };

async function importGeneratorPublic() {
  return crypto.subtle.importKey(
    'raw',
    GENERATOR_PUBKEY_COMPRESSED,
    ECDSA,
    true,
    ['verify']
  );
}

async function generatePair() {
  return crypto.subtle.generateKey(ECDSA, true, ['sign', 'verify']);
}

export const importRawCompressedPublicKey = {
  async test() {
    const key = await importGeneratorPublic();
    strictEqual(key.type, 'public');
    strictEqual(key.algorithm.name, 'ECDSA');
    strictEqual(key.algorithm.namedCurve, 'secp256k1');
    ok(key.usages.includes('verify'));
    ok(!key.usages.includes('sign'));
  },
};

export const importCompressedExportUncompressed = {
  // WebCrypto raw export is uncompressed SEC1; compressed input is accepted on import.
  async test() {
    const key = await importGeneratorPublic();
    const exported = new Uint8Array(await crypto.subtle.exportKey('raw', key));
    deepStrictEqual(exported, GENERATOR_PUBKEY_UNCOMPRESSED);
  },
};

export const importUncompressedRoundTrip = {
  async test() {
    const key = await crypto.subtle.importKey(
      'raw',
      GENERATOR_PUBKEY_UNCOMPRESSED,
      ECDSA,
      true,
      ['verify']
    );
    const exported = new Uint8Array(await crypto.subtle.exportKey('raw', key));
    deepStrictEqual(exported, GENERATOR_PUBKEY_UNCOMPRESSED);
  },
};

export const importRejectsWrongLength = {
  async test() {
    await rejects(
      crypto.subtle.importKey('raw', new Uint8Array(32), ECDSA, true, [
        'verify',
      ]),
      { name: 'DataError' }
    );
  },
};

export const importRejectsInvalidPoint = {
  async test() {
    // 0x02 prefix claims compressed even-y, but the rest isn't on the curve.
    const bogus = new Uint8Array(33);
    bogus[0] = 0x02;
    bogus.fill(0xff, 1);
    await rejects(
      crypto.subtle.importKey('raw', bogus, ECDSA, true, ['verify']),
      { name: 'DataError' }
    );
  },
};

export const generateKeyProducesPair = {
  async test() {
    const pair = await generatePair();
    strictEqual(pair.publicKey.type, 'public');
    strictEqual(pair.privateKey.type, 'private');
    strictEqual(pair.publicKey.algorithm.namedCurve, 'secp256k1');
    strictEqual(pair.privateKey.algorithm.namedCurve, 'secp256k1');
    ok(pair.privateKey.usages.includes('sign'));
    ok(pair.publicKey.usages.includes('verify'));
    ok(!pair.privateKey.usages.includes('verify'));
    ok(!pair.publicKey.usages.includes('sign'));
  },
};

export const signVerifyRoundTrip = {
  async test() {
    const { privateKey, publicKey } = await generatePair();
    const message = new TextEncoder().encode('hello secp256k1');
    const signature = await crypto.subtle.sign(
      ECDSA_SHA256,
      privateKey,
      message
    );
    strictEqual(signature.byteLength, 64);
    strictEqual(
      await crypto.subtle.verify(ECDSA_SHA256, publicKey, signature, message),
      true
    );

    const tampered = new Uint8Array(message);
    tampered[0] ^= 0x01;
    strictEqual(
      await crypto.subtle.verify(ECDSA_SHA256, publicKey, signature, tampered),
      false
    );
  },
};

export const signIsDeterministic = {
  // libsecp256k1 uses RFC 6979 deterministic nonces; same key + message must produce the same
  // signature bytes.
  async test() {
    const { privateKey } = await generatePair();
    const message = new TextEncoder().encode('determinism test');
    const sig1 = new Uint8Array(
      await crypto.subtle.sign(ECDSA_SHA256, privateKey, message)
    );
    const sig2 = new Uint8Array(
      await crypto.subtle.sign(ECDSA_SHA256, privateKey, message)
    );
    deepStrictEqual(sig1, sig2);
  },
};

export const signRejectsPublicKey = {
  async test() {
    const { publicKey } = await generatePair();
    await rejects(
      crypto.subtle.sign(ECDSA_SHA256, publicKey, new Uint8Array([0])),
      { name: 'InvalidAccessError' }
    );
  },
};

export const verifyRejectsWrongLengthSignature = {
  async test() {
    const key = await importGeneratorPublic();
    strictEqual(
      await crypto.subtle.verify(
        ECDSA_SHA256,
        key,
        new Uint8Array(63),
        new Uint8Array([0])
      ),
      false
    );
  },
};

export const verifyRejectsAllZeroSignature = {
  async test() {
    const key = await importGeneratorPublic();
    strictEqual(
      await crypto.subtle.verify(
        ECDSA_SHA256,
        key,
        new Uint8Array(64),
        new Uint8Array([0])
      ),
      false
    );
  },
};

// JWK assertions derive known-good JWKs by round-tripping through raw/generate paths and mutate
// from there, rather than hardcoding base64url coordinate strings.

async function makeGeneratorJwk() {
  const key = await importGeneratorPublic();
  return crypto.subtle.exportKey('jwk', key);
}

export const exportJwkShape = {
  async test() {
    const jwk = await makeGeneratorJwk();
    strictEqual(jwk.kty, 'EC');
    strictEqual(jwk.crv, 'secp256k1');
    strictEqual(typeof jwk.x, 'string');
    strictEqual(typeof jwk.y, 'string');
    strictEqual(jwk.d, undefined);
    strictEqual(jwk.ext, true);
    ok(jwk.key_ops.includes('verify'));
    // base64url of 32 bytes is 43 chars, alphabet [A-Za-z0-9_-].
    strictEqual(jwk.x.length, 43);
    strictEqual(jwk.y.length, 43);
    ok(/^[A-Za-z0-9_-]+$/.test(jwk.x));
    ok(/^[A-Za-z0-9_-]+$/.test(jwk.y));
  },
};

export const importJwkPublic = {
  async test() {
    const jwk = await makeGeneratorJwk();
    const key = await crypto.subtle.importKey('jwk', jwk, ECDSA, true, [
      'verify',
    ]);
    strictEqual(key.type, 'public');
    strictEqual(key.algorithm.namedCurve, 'secp256k1');
    ok(key.usages.includes('verify'));
  },
};

export const jwkRoundTrip = {
  async test() {
    const fromRaw = await importGeneratorPublic();
    const jwk = await crypto.subtle.exportKey('jwk', fromRaw);
    const fromJwk = await crypto.subtle.importKey('jwk', jwk, ECDSA, true, [
      'verify',
    ]);
    const reExported = new Uint8Array(
      await crypto.subtle.exportKey('raw', fromJwk)
    );
    deepStrictEqual(reExported, GENERATOR_PUBKEY_UNCOMPRESSED);
  },
};

export const importJwkRejectsWrongKty = {
  async test() {
    const jwk = await makeGeneratorJwk();
    jwk.kty = 'OKP';
    await rejects(
      crypto.subtle.importKey('jwk', jwk, ECDSA, true, ['verify']),
      { name: 'DataError' }
    );
  },
};

export const importJwkRejectsWrongCrv = {
  async test() {
    const jwk = await makeGeneratorJwk();
    jwk.crv = 'P-256';
    await rejects(
      crypto.subtle.importKey('jwk', jwk, ECDSA, true, ['verify']),
      { name: 'DataError' }
    );
  },
};

export const importJwkRejectsWrongAlg = {
  // RFC 8812: if `alg` is present on a secp256k1 JWK it must be "ES256K".
  async test() {
    const jwk = await makeGeneratorJwk();
    jwk.alg = 'ES256';
    await rejects(
      crypto.subtle.importKey('jwk', jwk, ECDSA, true, ['verify']),
      { name: 'DataError' }
    );
  },
};

export const importJwkAcceptsShortCoordinate = {
  // RFC 7518 permits omitting leading zero bytes in `d`. The generator point's private scalar is
  // 1, encoded minimally as a single 0x01 byte ("AQ" in base64url). Our impl zero-pads to 32
  // bytes; the import should succeed and the resulting key should sign in a way the original
  // generator public key can verify.
  async test() {
    const generatorJwk = await makeGeneratorJwk();
    const shortPrivateJwk = {
      kty: 'EC',
      crv: 'secp256k1',
      x: generatorJwk.x,
      y: generatorJwk.y,
      d: 'AQ',
      ext: true,
    };
    const privateKey = await crypto.subtle.importKey(
      'jwk',
      shortPrivateJwk,
      ECDSA,
      true,
      ['sign']
    );
    const publicKey = await importGeneratorPublic();
    const message = new TextEncoder().encode('short-d test');
    const sig = await crypto.subtle.sign(ECDSA_SHA256, privateKey, message);
    strictEqual(
      await crypto.subtle.verify(ECDSA_SHA256, publicKey, sig, message),
      true
    );
  },
};

export const importJwkRejectsOutOfRangePrivateScalar = {
  // d = 0xFF...FF is greater than the secp256k1 curve order n, so secp256k1_ec_seckey_verify
  // must reject it.
  async test() {
    const generatorJwk = await makeGeneratorJwk();
    const allOnes = new Uint8Array(32).fill(0xff);
    const dB64 = btoa(String.fromCharCode(...allOnes))
      .replace(/\+/g, '-')
      .replace(/\//g, '_')
      .replace(/=+$/, '');
    const badJwk = {
      kty: 'EC',
      crv: 'secp256k1',
      x: generatorJwk.x,
      y: generatorJwk.y,
      d: dB64,
      ext: true,
    };
    await rejects(
      crypto.subtle.importKey('jwk', badJwk, ECDSA, true, ['sign']),
      { name: 'DataError' }
    );
  },
};

export const jwkPrivateRoundTrip = {
  async test() {
    const { privateKey, publicKey } = await generatePair();
    const jwk = await crypto.subtle.exportKey('jwk', privateKey);
    strictEqual(jwk.kty, 'EC');
    strictEqual(jwk.crv, 'secp256k1');
    strictEqual(typeof jwk.d, 'string');
    strictEqual(jwk.d.length, 43);
    ok(jwk.key_ops.includes('sign'));

    const reimported = await crypto.subtle.importKey('jwk', jwk, ECDSA, true, [
      'sign',
    ]);
    strictEqual(reimported.type, 'private');

    const message = new TextEncoder().encode('round-trip via JWK');
    const sig = await crypto.subtle.sign(ECDSA_SHA256, reimported, message);
    strictEqual(
      await crypto.subtle.verify(ECDSA_SHA256, publicKey, sig, message),
      true
    );
  },
};

export const jwkPrivateRejectsMismatchedHalves = {
  // A JWK whose (x, y) doesn't match the point derived from `d` must be rejected, not silently
  // produce a broken key.
  async test() {
    const a = await generatePair();
    const b = await generatePair();
    const jwkA = await crypto.subtle.exportKey('jwk', a.privateKey);
    const jwkB = await crypto.subtle.exportKey('jwk', b.privateKey);
    const mixed = { ...jwkA, d: jwkB.d };
    await rejects(
      crypto.subtle.importKey('jwk', mixed, ECDSA, true, ['sign']),
      {
        name: 'DataError',
        message: /inconsistent|do not match/,
      }
    );
  },
};

export const ecdhRejectedExplicitly = {
  async test() {
    await rejects(
      crypto.subtle.importKey(
        'raw',
        GENERATOR_PUBKEY_COMPRESSED,
        { name: 'ECDH', namedCurve: 'secp256k1' },
        true,
        ['deriveBits']
      ),
      {
        name: 'NotSupportedError',
        message: /ECDH is not supported for curve "secp256k1"/,
      }
    );
  },
};

export const ecdhGenerateRejectedExplicitly = {
  // secp256k1 over ECDH must reject at generateKey too, not just at importKey.
  async test() {
    await rejects(
      crypto.subtle.generateKey(
        { name: 'ECDH', namedCurve: 'secp256k1' },
        true,
        ['deriveBits']
      ),
      { name: 'NotSupportedError' }
    );
  },
};
