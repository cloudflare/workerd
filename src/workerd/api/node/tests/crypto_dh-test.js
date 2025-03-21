// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
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
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

'use strict';

import { Buffer } from 'node:buffer';

import * as assert from 'node:assert';
import * as crypto from 'node:crypto';

export const dh_test = {
  test(ctrl, env, ctx) {
    // https://github.com/nodejs/node/issues/32738
    // XXX(bnoordhuis) validateInt32() throwing ERR_OUT_OF_RANGE and RangeError
    // instead of ERR_INVALID_ARG_TYPE and TypeError is questionable, IMO.
    assert.throws(() => crypto.createDiffieHellman(13.37), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
      message:
        'The value of "sizeOrKey" is out of range. ' +
        'It must be an integer. Received 13.37',
    });

    assert.throws(() => crypto.createDiffieHellman('abcdef', 13.37), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
      message:
        'The value of "generator" is out of range. ' +
        'It must be an integer. Received 13.37',
    });
    // BoringSSL throws when key sizes > 10000 bits are requested, we proactively return a
    // RangeError instead which is more descriptive.
    assert.throws(() => crypto.createDiffieHellman(10001), {
      name: 'RangeError',
    });

    for (const bits of [-1, 0, 1]) {
      assert.throws(() => crypto.createDiffieHellman(bits), {
        name: 'Error',
      });
    }

    // Through a fluke of history, g=0 defaults to DH_GENERATOR (2).
    {
      const g = 0;
      crypto.createDiffieHellman('abcdef', 'hex', g);
    }

    for (const g of [-1, 1]) {
      const ex = {
        name: 'RangeError',
      };
      assert.throws(() => crypto.createDiffieHellman('abcdef', g), ex);
      assert.throws(() => crypto.createDiffieHellman('abcdef', 'hex', g), ex);
    }

    // Calls with even p, or p values that are interpreted as <= g will be rejected.
    crypto.createDiffieHellman('abcdef', 'hex', Buffer.from([2])); // OK
    for (const bits of ['abcdef', Buffer.from([1]), Buffer.from([4])]) {
      assert.throws(() => crypto.createDiffieHellman(bits), {
        name: 'Error',
      });
    }

    for (const g of [Buffer.from([]), Buffer.from([0]), Buffer.from([1])]) {
      const ex = {
        name: 'Error',
      };
      assert.throws(() => crypto.createDiffieHellman('abcdef', g), ex);
      assert.throws(() => crypto.createDiffieHellman('abcdef', 'hex', g), ex);
    }

    [[0x1, 0x2], () => {}, /abc/, {}].forEach((input) => {
      assert.throws(() => crypto.createDiffieHellman(input), {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      });
    });

    assert.throws(
      function () {
        crypto.getDiffieHellman('unknown-group');
      },
      {
        name: 'Error',
      },
      "crypto.getDiffieHellman('unknown-group') " +
        'failed to throw the expected error.'
    );

    assert.throws(() => crypto.createDiffieHellman('', true), {
      code: 'ERR_INVALID_ARG_TYPE',
    });

    [true, Symbol(), {}, () => {}, []].forEach((generator) =>
      assert.throws(() => crypto.createDiffieHellman('', 'base64', generator), {
        name: 'TypeError',
      })
    );
  },
};

///////////////

export const dh_verify_error_test = {
  test(ctrl, env, ctx) {
    // Second OAKLEY group, see
    // https://github.com/nodejs/node-v0.x-archive/issues/2338 and
    // https://xml2rfc.tools.ietf.org/public/rfc/html/rfc2412.html#anchor49
    const p =
      'FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74' +
      '020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437' +
      '4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED' +
      'EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE65381FFFFFFFFFFFFFFFF';
    crypto.createDiffieHellman(p, 'hex');

    // Confirm DH_check() results are exposed for optional examination. Use an odd value for prime
    // since even values are rejected immediately.
    const bad_dh = crypto.createDiffieHellman('03', 'hex');
    assert.notStrictEqual(bad_dh.verifyError, 0);
  },
};

/////////////////////

export const dh_constructor_test = {
  test(ctrl, env, ctx) {
    const DiffieHellmanGroup = crypto.DiffieHellmanGroup;
    const dhg = DiffieHellmanGroup('modp14');
    assert.ok(
      dhg instanceof DiffieHellmanGroup,
      'DiffieHellmanGroup is expected ' +
        'to return a new instance when ' +
        'called without `new`'
    );

    const p1 = dhg.getPrime('buffer');
    const DiffieHellman = crypto.DiffieHellman;
    const dh = DiffieHellman(p1, 'buffer');
    assert.ok(
      dh instanceof DiffieHellman,
      'DiffieHellman is expected to return a ' +
        'new instance when called without `new`'
    );
  },
};
////////////////////

// This test will fail if BoringSSL runs in FIPS mode and succeed otherwise; disable it for now.

/*
function test() {
  const odd = Buffer.alloc(39, 'A');

  const c = crypto.createDiffieHellman(size);
  c.setPrivateKey(odd);
  c.generateKeys();
}

// FIPS requires a length of at least 1024
if (!common.hasFipsCrypto) {
  test();
} else {
  assert.throws(function() { test(); }, /key size too small/);
}
*/

//////////////////

export const dh_group_test = {
  test(ctrl, env, ctx) {
    assert.throws(
      function () {
        crypto.getDiffieHellman('modp14').setPrivateKey('');
      },
      new RegExp(
        '^TypeError: crypto\\.getDiffieHellman\\(\\.\\.\\.\\)\\.' +
          'setPrivateKey is not a function$'
      ),
      "crypto.getDiffieHellman('modp14').setPrivateKey('') " +
        'failed to throw the expected error.'
    );
    assert.throws(
      function () {
        crypto.getDiffieHellman('modp14').setPublicKey('');
      },
      new RegExp(
        '^TypeError: crypto\\.getDiffieHellman\\(\\.\\.\\.\\)\\.' +
          'setPublicKey is not a function$'
      ),
      "crypto.getDiffieHellman('modp14').setPublicKey('') " +
        'failed to throw the expected error.'
    );
  },
};

////////////////

export const dh_exchange_test = {
  test(ctrl, env, ctx) {
    const alice = crypto.createDiffieHellmanGroup('modp14');
    const bob = crypto.createDiffieHellmanGroup('modp14');
    alice.generateKeys();
    bob.generateKeys();
    const aSecret = alice.computeSecret(bob.getPublicKey()).toString('hex');
    const bSecret = bob.computeSecret(alice.getPublicKey()).toString('hex');
    assert.strictEqual(aSecret, bSecret);
  },
};

////////////////

// This test verifies padding with leading zeroes for shared
// secrets that are strictly smaller than the modulus (prime).
// See:
//  RFC 4346: https://www.ietf.org/rfc/rfc4346.txt
//  https://github.com/nodejs/node-v0.x-archive/issues/7906
//  https://github.com/nodejs/node-v0.x-archive/issues/5239
//
// In FIPS mode OPENSSL_DH_FIPS_MIN_MODULUS_BITS = 1024, meaning we need
// a FIPS-friendly >= 1024 bit prime, we can use MODP 14 from RFC 3526:
// https://www.ietf.org/rfc/rfc3526.txt
//
// We can generate appropriate values with this code:
//
// crypto = require('crypto');
//
// for (;;) {
//   var a = crypto.getDiffieHellman('modp14'),
//   var b = crypto.getDiffieHellman('modp14');
//
//   a.generateKeys();
//   b.generateKeys();
//
//   var aSecret = a.computeSecret(b.getPublicKey()).toString('hex');
//   console.log("A public: " + a.getPublicKey().toString('hex'));
//   console.log("A private: " + a.getPrivateKey().toString('hex'));
//   console.log("B public: " + b.getPublicKey().toString('hex'));
//   console.log("B private: " + b.getPrivateKey().toString('hex'));
//   console.log("A secret: " + aSecret);
//   console.log('-------------------------------------------------');
//   if(aSecret.substring(0,2) === "00") {
//     console.log("found short key!");
//     return;
//   }
// }

const apub =
  '5484455905d3eff34c70980e871f27f05448e66f5a6efbb97cbcba4e927196c2bd9ea272cded91\
10a4977afa8d9b16c9139a444ed2d954a794650e5d7cb525204f385e1af81530518563822ecd0f9\
524a958d02b3c269e79d6d69850f0968ad567a4404fbb0b19efc8bc73e267b6136b88cafb33299f\
f7c7cace3ffab1a88c2c9ee841f88b4c3679b4efc465f5c93cca11d487be57373e4c5926f634c4e\
efee6721d01db91cd66321615b2522f96368dbc818875d422140d0edf30bdb97d9721feddcb9ff6\
453741a4f687ee46fc54bf1198801f1210ac789879a5ee123f79e2d2ce1209df2445d32166bc9e4\
8f89e944ec9c3b2e16c8066cd8eebd4e33eb941';
const bpub =
  '3fca64510e36bc7da8a3a901c7b74c2eabfa25deaf7cbe1d0c50235866136ad677317279e1fb0\
06e9c0a07f63e14a3363c8e016fbbde2b2c7e79fed1cc3e08e95f7459f547a8cd0523ee9dc744d\
e5a956d92b937db4448917e1f6829437f05e408ee7aea70c0362b37370c7c75d14449d8b2d2133\
04ac972302d349975e2265ca7103cfebd019d9e91234d638611abd049014f7abf706c1c5da6c88\
788a1fdc6cdf17f5fffaf024ce8711a2ebde0b52e9f1cb56224483826d6e5ac6ecfaae07b75d20\
6e8ac97f5be1a5b68f20382f2a7dac189cf169325c4cf845b26a0cd616c31fec905c5d9035e5f7\
8e9880c812374ac0f3ca3d365f06e4be526b5affd4b79';
const apriv =
  '62411e34704637d99c6c958a7db32ac22fcafafbe1c33d2cfdb76e12ded41f38fc16b792b9041\
2e4c82755a3815ba52f780f0ee296ad46e348fc4d1dcd6b64f4eea1b231b2b7d95c5b1c2e26d34\
83520558b9860a6eb668f01422a54e6604aa7702b4e67511397ef3ecb912bff1a83899c5a5bfb2\
0ee29249a91b8a698e62486f7009a0e9eaebda69d77ecfa2ca6ba2db6c8aa81759c8c90c675979\
08c3b3e6fc60668f7be81cce6784482af228dd7f489005253a165e292802cfd0399924f6c56827\
7012f68255207722355634290acc7fddeefbba75650a85ece95b6a12de67eac016ba78960108dd\
5dbadfaa43cc9fed515a1f307b7d90ae0623bc7b8cefb';
const secret =
  '00c37b1e06a436d6717816a40e6d72907a6f255638b93032267dcb9a5f0b4a9aa0236f3dce63b\
1c418c60978a00acd1617dfeecf1661d8a3fafb4d0d8824386750f4853313400e7e4afd22847e4\
fa56bc9713872021265111906673b38db83d10cbfa1dea3b6b4c97c8655f4ae82125281af7f234\
8916a15c6f95649367d169d587697480df4d10b381479e86d5518b520d9d8fb764084eab518224\
dc8fe984ddaf532fc1531ce43155fa0ab32532bf1ece5356b8a3447b5267798a904f16f3f4e635\
597adc0179d011132dcffc0bbcb0dd2c8700872f8663ec7ddd897c659cc2efebccc73f38f0ec96\
8612314311231f905f91c63a1aea52e0b60cead8b57df';

export const dh_padding_test = {
  test(ctrl, env, ctx) {
    /* FIPS-friendly 2048 bit prime */
    const p = crypto.createDiffieHellman(
      crypto.getDiffieHellman('modp14').getPrime()
    );

    p.setPublicKey(apub, 'hex');
    p.setPrivateKey(apriv, 'hex');

    assert.strictEqual(
      p.computeSecret(bpub, 'hex', 'hex').toString('hex'),
      secret
    );
  },
};

export const dhKeygenTest = {
  test() {
    // FIPS-mode BoringSSL mandates keys of at least 1024 bits. RFC 8270 recommends that sizes of
    // at least 2048 bits should be used, 1024-bit primes are sufficient for these tests though.
    const size = 1024;
    const dh1 = crypto.createDiffieHellman(size);
    const p1 = dh1.getPrime('buffer');
    const dh2 = crypto.createDiffieHellman(p1, 'buffer');
    const key1 = dh1.generateKeys();
    const key2 = dh2.generateKeys('hex');
    const secret1 = dh1.computeSecret(key2, 'hex', 'base64');
    const secret2 = dh2.computeSecret(key1, 'latin1', 'buffer');

    // Test Diffie-Hellman with two parties sharing a secret,
    // using various encodings as we go along
    assert.strictEqual(secret2.toString('base64'), secret1);

    assert.strictEqual(dh1.verifyError, 0);
    assert.strictEqual(dh2.verifyError, 0);

    // Create "another dh1" using generated keys from dh1,
    // and compute secret again
    const dh3 = crypto.createDiffieHellman(p1, 'buffer');
    const privkey1 = dh1.getPrivateKey();
    dh3.setPublicKey(key1);
    dh3.setPrivateKey(privkey1);

    assert.deepStrictEqual(dh1.getPrime(), dh3.getPrime());
    assert.deepStrictEqual(dh1.getGenerator(), dh3.getGenerator());
    assert.deepStrictEqual(dh1.getPublicKey(), dh3.getPublicKey());
    assert.deepStrictEqual(dh1.getPrivateKey(), dh3.getPrivateKey());
    assert.strictEqual(dh3.verifyError, 0);

    const secret3 = dh3.computeSecret(key2, 'hex', 'base64');

    assert.strictEqual(secret1, secret3);

    // computeSecret works without a public key set at all.
    const dh4 = crypto.createDiffieHellman(p1, 'buffer');
    dh4.setPrivateKey(privkey1);

    assert.deepStrictEqual(dh1.getPrime(), dh4.getPrime());
    assert.deepStrictEqual(dh1.getGenerator(), dh4.getGenerator());
    assert.deepStrictEqual(dh1.getPrivateKey(), dh4.getPrivateKey());
    assert.strictEqual(dh4.verifyError, 0);

    const secret4 = dh4.computeSecret(key2, 'hex', 'base64');

    assert.strictEqual(secret1, secret4);

    assert.throws(
      () => {
        dh3.computeSecret('');
      },
      { name: 'Error' }
    );
  },
};

export const ecdh = {
  test() {
    const curves = crypto.getCurves();
    curves.forEach((i) => {
      const alice = crypto.createECDH(i);
      const bob = crypto.createECDH(i);

      alice.generateKeys();
      bob.generateKeys();

      const aliceSecret = alice.computeSecret(bob.getPublicKey(), null, 'hex');
      const bobSecret = bob.computeSecret(alice.getPublicKey(), null, 'hex');

      assert.strictEqual(aliceSecret, bobSecret);
    });
  },
};

export const ecdhConvertKey = {
  test() {
    const ecdh = crypto.createECDH('prime256v1');
    ecdh.generateKeys();

    const compressedKey = ecdh.getPublicKey('hex', 'compressed');

    const uncompressedKey = crypto.ECDH.convertKey(
      compressedKey,
      'prime256v1',
      'hex',
      'hex',
      'uncompressed'
    );

    // The converted key and the uncompressed public key should be the same
    assert.strictEqual(uncompressedKey, ecdh.getPublicKey('hex'));
  },
};

export const statelessDh = {
  test() {
    // DH keygen is currently unsupported by the boringssl+fips
    // we use internally. This test works for workerd but fails
    // on the internal run.
    // const pair1 = crypto.generateKeyPairSync('dh', {
    //   prime: Buffer.from([31, 0, 0, 0, 0, 1]),
    // });
    // const pair2 = crypto.generateKeyPairSync('dh', {
    //   prime: Buffer.from([31, 0, 0, 0, 0, 1]),
    // });
    // const sec1 = crypto.diffieHellman({
    //   publicKey: pair1.publicKey,
    //   privateKey: pair2.privateKey,
    // });
    // const sec2 = crypto.diffieHellman({
    //   publicKey: pair2.publicKey,
    //   privateKey: pair1.privateKey,
    // });
    // assert.deepStrictEqual(sec1, sec2);
  },
};

export const statelessDhX25591 = {
  test() {
    const pair1 = crypto.generateKeyPairSync('x25519');
    const pair2 = crypto.generateKeyPairSync('x25519');
    const sec1 = crypto.diffieHellman({
      publicKey: pair1.publicKey,
      privateKey: pair2.privateKey,
    });
    const sec2 = crypto.diffieHellman({
      publicKey: pair2.publicKey,
      privateKey: pair1.privateKey,
    });
    assert.deepStrictEqual(sec1, sec2);
  },
};

export const statelessDhEc = {
  test() {
    // The Boringssl-based implementation currently does not support
    // the mechanisms for stateless dh using named EC curve.
    const pair1 = crypto.generateKeyPairSync('ec', { namedCurve: 'secp224r1' });
    const pair2 = crypto.generateKeyPairSync('ec', { namedCurve: 'secp224r1' });
    assert.throws(
      () => {
        crypto.diffieHellman({
          publicKey: pair1.publicKey,
          privateKey: pair2.privateKey,
        });
      },
      {
        message: /Failed to derive/,
      }
    );
  },
};
