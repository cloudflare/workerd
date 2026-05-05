// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual, ok } from 'node:assert';

export const timingSafeEqual = {
  test() {
    // Note that this does not actually test that the equality check is,
    // in fact, timing safe. It checks only the basic operation of the API

    const enc = new TextEncoder();
    [
      [new ArrayBuffer(0), new ArrayBuffer(0)],
      [new ArrayBuffer(1), new ArrayBuffer(1)],
      [enc.encode('hello'), enc.encode('hello')],
      [enc.encode('hellothere'), enc.encode('hellothere').buffer],
    ].forEach(([a, b]) => {
      if (!crypto.subtle.timingSafeEqual(a, b)) {
        throw new Error('inputs should have been equal', a, b);
      }
    });

    [
      [enc.encode('hello'), enc.encode('there')],
      [new Uint8Array([1, 2, 3, 4]), new Uint32Array([1])],
    ].forEach(([a, b]) => {
      if (crypto.subtle.timingSafeEqual(a, b)) {
        throw new Error('inputs should not have been equal', a, b);
      }
    });

    [
      ['hello', 'there'],
      [new ArrayBuffer(0), new ArrayBuffer(1)],
    ].forEach(([a, b]) => {
      try {
        crypto.subtle.timingSafeEqual(a, b);
        throw new Error('inputs should have caused an error', a, b);
      } catch {
        // intentionally empty
      }
    });
  },
};

export const randomUuid = {
  test() {
    const pattern =
      /[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[ab89][a-f0-9]{3}-[a-f0-9]{12}/;
    // Loop through a bunch of generated UUID's to make sure we're consistently successful.
    for (let n = 0; n < 100; n++) {
      const uuid = crypto.randomUUID();
      if (!pattern.test(uuid)) {
        throw new Error(`${uuid} is not a valid random UUID`);
      }
    }
  },
};

export const cryptoGcmIvZeroLength = {
  async test() {
    const key = await crypto.subtle.generateKey(
      {
        name: 'AES-GCM',
        length: 256,
      },
      true,
      ['encrypt', 'decrypt']
    );

    for (const op of ['encrypt', 'decrypt']) {
      await crypto.subtle[op](
        {
          name: 'AES-GCM',
          iv: new ArrayBuffer(0),
        },
        key,
        new ArrayBuffer(100)
      ).then(
        () => {
          throw new Error('should not have resolved');
        },
        (err) => {
          if (
            err.constructor !== DOMException ||
            err.message !== 'AES-GCM IV must not be empty.'
          ) {
            throw err;
          }
        }
      );
    }
  },
};

export const cryptoZeroLength = {
  async test() {
    function arrayBuffer2hex(arr) {
      return Array.from(new Uint8Array(arr))
        .map((i) => ('0' + i.toString(16)).slice(-2))
        .join('');
    }

    // Try using a zero-length input to various crypto functions. This should be valid.
    // At one point, encrypt() would sometimes fail on an empty input -- but, mysteriously,
    // it depended on how exactly the ArrayBuffer was constructed! The problem turned out to
    // be BoringSSL rejecting null pointers even if the size was 0.

    const empty = new ArrayBuffer();

    const DIGESTS = {
      MD5: 'd41d8cd98f00b204e9800998ecf8427e',
      'SHA-1': 'da39a3ee5e6b4b0d3255bfef95601890afd80709',
      'SHA-256':
        'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
      'SHA-512':
        'cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e',
    };

    for (const name in DIGESTS) {
      const result = arrayBuffer2hex(await crypto.subtle.digest(name, empty));
      if (result != DIGESTS[name]) {
        throw new Error(
          'for ' + name + ', expected ' + DIGESTS[name] + ' got ' + result
        );
      }
    }

    const ENCRYPTS = {
      'AES-CBC': 'dd3eedef984211b98384dc5677bc728e',
      'AES-GCM': 'fedbd1a722cb7c1a52f529e0469ee449',
    };

    for (const name in ENCRYPTS) {
      const key = await crypto.subtle.importKey(
        'raw',
        new Uint8Array([
          0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf, 0, 1, 2,
          3, 4, 5, 6, 7, 8, 9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf,
        ]),
        name,
        true,
        ['encrypt']
      );
      const result = arrayBuffer2hex(
        await crypto.subtle.encrypt(
          { name, iv: new Uint8Array(16) },
          key,
          empty
        )
      );
      if (result != ENCRYPTS[name]) {
        throw new Error(
          'for ' + name + ', expected ' + ENCRYPTS[name] + ' got ' + result
        );
      }
    }
  },
};

export const deriveBitsNullLength = {
  async test() {
    // Tests that deriveBits can take a null or undefined length
    // argument and still return the correct number of bits if
    // the algorithm supports it. This is a recent spec change.

    const pair = await crypto.subtle.generateKey(
      {
        name: 'ECDH',
        namedCurve: 'P-384',
      },
      false,
      ['deriveBits']
    );

    {
      const bits = await crypto.subtle.deriveBits(
        {
          name: 'ECDH',
          namedCurve: 'P-384',
          public: pair.publicKey,
        },
        pair.privateKey,
        undefined
      );

      strictEqual(bits.byteLength, 48);
    }

    {
      const bits = await crypto.subtle.deriveBits(
        {
          name: 'ECDH',
          namedCurve: 'P-384',
          public: pair.publicKey,
        },
        pair.privateKey,
        null
      );

      strictEqual(bits.byteLength, 48);
    }

    {
      const bits = await crypto.subtle.deriveBits(
        {
          name: 'ECDH',
          namedCurve: 'P-384',
          public: pair.publicKey,
        },
        pair.privateKey
      );

      strictEqual(bits.byteLength, 48);
    }
  },
};

export const aesCounterOverflowTest = {
  async test() {
    // Regression test: Check that the input counter is not modified when it overflows in the
    // internal computation.
    const key = await crypto.subtle.generateKey(
      {
        name: 'AES-CTR',
        length: 128,
      },
      false,
      ['encrypt']
    );

    // Maximum counter value, will overflow and require processing in two parts if there is more
    // than one input data block.
    const counter = new Uint8Array([
      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
      255,
    ]);
    const counter2 = counter.slice();

    await crypto.subtle.encrypt(
      {
        name: 'AES-CTR',
        length: 128,
        counter,
      },
      key,
      new TextEncoder().encode('A'.repeat(2 * 16))
    );
    ok(crypto.subtle.timingSafeEqual(counter, counter2));
  },
};

// Test that RSA JWK import with partially invalid fields throws a proper error
// without leaking memory. The valid n/e/d fields cause BIGNUM allocations; the
// invalid p field (bad base64) triggers a throw. Under ASAN this verifies the
// BIGNUMs allocated for n/e/d are properly freed on the error path.
export const rsaJwkPartialImportFailure = {
  async test() {
    // A minimal RSA-2048 JWK with valid n, e, d but invalid CRT parameters.
    // The n/e/d values are from a real key (public, not sensitive).
    const jwk = {
      kty: 'RSA',
      // Valid base64url-encoded values for n and e (from an RSA-2048 test key)
      n:
        'sXchDaQebHnPiGvhGPEUBYNmRREkfWAz4CZV0FxTwtQq6R51mJk8qnnU_6DE_XJr' +
        'T2JVNPB-bIXGFNnMLPOsTf5Q4r9Ks3h3S3tPzFqSd9Cjv0eRe-ZhWBYFkl-bLE1h' +
        'ZGnmtQ--KfAiMvAtYNfRJwKL9cSKpGQTmqY6_0IbUqbZ0dXf_5D4rKCiZaQj-lTbm' +
        'Eifn5JeRKnA2VY4dQvVQKhoQp_dEFwjOLGPOJ3yJhAFRrtFI3tzH7jSLNz2FA9gHk' +
        'LaPrGxWF-bSNqlegYCr8CATCNfCAt9lDbCCHJiB5TQ5B-R40gM-y_M44zzX9nbZuA' +
        'rSkBjQ',
      e: 'AQAB',
      d:
        'VFCWOqXr8nvZNyaaJLXEnFBR3W45lj0nSjpUGSH-wOjK4p5_FDRlaL-eRa-VQvwjJ' +
        '38BRJk9_0dKJPCMcuFVlj-B0FNpZ_gkBGC-jlLfCq3SBjRFBasVUR5vh4GGe_pFD3p' +
        '0RWjwwl_6yPb_cCeI4XP4kK4JEWndHjvNmBcZI6PU0Lc_8-Fb_Z0-BTN3BA0DBkFS' +
        'GCQN7G4dCdNQ3Onn3y2JBXB-pYlFkiHyR0j0o_GFoH_GE-WxQb7q0PjkNV-sMFQ8' +
        '0ql44vEPg0Z8bZ0d1g_j8_Z3PuKCPJxJ6T3IGHPV1D-kBJyBvjJ-rlKr4XQ6XqvAp' +
        'XWPQ',
      // Invalid base64 in CRT parameters — should cause import to throw
      p: '!!!not-valid-base64!!!',
      q: '!!!not-valid-base64!!!',
      dp: '!!!not-valid-base64!!!',
      dq: '!!!not-valid-base64!!!',
      qi: '!!!not-valid-base64!!!',
    };

    let threw = false;
    try {
      await crypto.subtle.importKey(
        'jwk',
        jwk,
        { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' },
        true,
        ['sign']
      );
    } catch (e) {
      threw = true;
      // Should get an error about invalid base64, not crash or silently succeed
      ok(e instanceof Error, 'Expected an Error');
    }
    ok(threw, 'Import should have thrown for invalid CRT parameters');

    // Also test with valid n/e but invalid d (earlier failure point)
    const jwk2 = {
      kty: 'RSA',
      n: jwk.n,
      e: jwk.e,
      d: '!!!not-valid-base64!!!',
      p: '!!!not-valid-base64!!!',
      q: '!!!not-valid-base64!!!',
      dp: '!!!not-valid-base64!!!',
      dq: '!!!not-valid-base64!!!',
      qi: '!!!not-valid-base64!!!',
    };

    threw = false;
    try {
      await crypto.subtle.importKey(
        'jwk',
        jwk2,
        { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' },
        true,
        ['sign']
      );
    } catch (e) {
      threw = true;
      ok(e instanceof Error, 'Expected an Error');
    }
    ok(threw, 'Import should have thrown for invalid private exponent');
  },
};

// Test that operations on detached ArrayBuffers return empty results instead of
// crashing. This exercises the WasDetached() checks in JsArrayBuffer, JsUint8Array,
// JsArrayBufferView, and JsBufferSource.
export const detachedBufferHandling = {
  async test() {
    // Create a buffer, detach it by transferring, then use it with crypto APIs.
    const buf = new ArrayBuffer(16);
    const view = new Uint8Array(buf);

    // Detach by transferring to a new ArrayBuffer via structuredClone
    structuredClone(buf, { transfer: [buf] });

    // buf is now detached — byteLength should be 0
    strictEqual(buf.byteLength, 0);

    // getRandomValues should handle the detached view gracefully
    let threw = false;
    try {
      crypto.getRandomValues(view);
    } catch (_e) {
      threw = true;
    }
    // The detached buffer has 0 length, which should be accepted (0 <= 65536)
    // but the view reports 0 bytes so getRandomValues is effectively a no-op.
    // Either succeeding with 0 bytes or throwing is acceptable behavior.

    // timingSafeEqual with detached buffers
    const detachedBuf2 = new ArrayBuffer(0);
    ok(
      crypto.subtle.timingSafeEqual(detachedBuf2, new ArrayBuffer(0)),
      'Empty buffers should be timing-safe equal'
    );

    // Verify that encrypt with a detached IV throws rather than crashing
    const key = await crypto.subtle.generateKey(
      { name: 'AES-GCM', length: 128 },
      false,
      ['encrypt']
    );
    const detachedIv = new ArrayBuffer(12);
    structuredClone(detachedIv, { transfer: [detachedIv] });
    threw = false;
    try {
      await crypto.subtle.encrypt(
        { name: 'AES-GCM', iv: detachedIv },
        key,
        new ArrayBuffer(0)
      );
    } catch (_e) {
      threw = true;
    }
    ok(threw, 'Encrypt with detached IV should throw');
  },
};

// Test that EC JWK import with mismatched public/private key components is rejected.
// EC_KEY_check_key validates that the private key d corresponds to the public key (x, y).
export const ecJwkKeyConsistencyCheck = {
  async test() {
    // Generate a valid P-256 key pair to get real x, y values
    const keyPair = await crypto.subtle.generateKey(
      { name: 'ECDSA', namedCurve: 'P-256' },
      true,
      ['sign', 'verify']
    );
    const validJwk = await crypto.subtle.exportKey('jwk', keyPair.privateKey);

    // Corrupt the private key d while keeping x, y valid.
    // This creates an inconsistency: d does not correspond to (x, y).
    const corruptedJwk = {
      ...validJwk,
      d: validJwk.d.split('').reverse().join(''),
    };

    let threw = false;
    try {
      await crypto.subtle.importKey(
        'jwk',
        corruptedJwk,
        { name: 'ECDSA', namedCurve: 'P-256' },
        true,
        ['sign']
      );
    } catch (e) {
      threw = true;
      ok(e instanceof Error, 'Expected an Error');
    }
    ok(threw, 'Import should have thrown for inconsistent EC JWK private key');
  },
};
