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

import {
  Buffer,
} from 'node:buffer';

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
      message: 'The value of "sizeOrKey" is out of range. ' +
              'It must be an integer. Received 13.37',
    });

    assert.throws(() => crypto.createDiffieHellman('abcdef', 13.37), {
      code: 'ERR_OUT_OF_RANGE',
      name: 'RangeError',
      message: 'The value of "generator" is out of range. ' +
              'It must be an integer. Received 13.37',
    });

    for (const bits of [-1, 0, 1]) {
      assert.throws(() => crypto.createDiffieHellman(bits), {
        name: 'Error',
      });
    }

    // Through a fluke of history, g=0 defaults to DH_GENERATOR (2).
    {
      const g = 0;
      crypto.createDiffieHellman('abcdef', g);
      crypto.createDiffieHellman('abcdef', 'hex', g);
    }

    for (const g of [-1, 1]) {
      const ex = {
        name: 'RangeError',
      };
      assert.throws(() => crypto.createDiffieHellman('abcdef', g), ex);
      assert.throws(() => crypto.createDiffieHellman('abcdef', 'hex', g), ex);
    }

    crypto.createDiffieHellman('abcdef', Buffer.from([2]));  // OK

    for (const g of [Buffer.from([]),
                    Buffer.from([0]),
                    Buffer.from([1])]) {
      const ex = {
        name: 'Error',
      };
      assert.throws(() => crypto.createDiffieHellman('abcdef', g), ex);
      assert.throws(() => crypto.createDiffieHellman('abcdef', 'hex', g), ex);
    }

    [
      [0x1, 0x2],
      () => { },
      /abc/,
      {},
    ].forEach((input) => {
      assert.throws(
        () => crypto.createDiffieHellman(input),
        {
          code: 'ERR_INVALID_ARG_TYPE',
          name: 'TypeError',
        }
      );
    });

    assert.throws(
      function() {
        crypto.getDiffieHellman('unknown-group');
      },
      {
        name: 'Error',
      },
      'crypto.getDiffieHellman(\'unknown-group\') ' +
      'failed to throw the expected error.'
    );

    assert.throws(
      () => crypto.createDiffieHellman('', true),
      {
        code: 'ERR_INVALID_ARG_TYPE'
      }
    );

    [true, Symbol(), {}, () => {}, []].forEach((generator) => assert.throws(
      () => crypto.createDiffieHellman('', 'base64', generator),
      { name: 'TypeError' }
    ));
  }
}

///////////////

export const dh_verify_error_test = {
  test(ctrl, env, ctx) {
// Second OAKLEY group, see
// https://github.com/nodejs/node-v0.x-archive/issues/2338 and
// https://xml2rfc.tools.ietf.org/public/rfc/html/rfc2412.html#anchor49
const p = 'FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74' +
          '020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437' +
          '4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED' +
          'EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE65381FFFFFFFFFFFFFFFF';
crypto.createDiffieHellman(p, 'hex');

// Confirm DH_check() results are exposed for optional examination.
const bad_dh = crypto.createDiffieHellman('02', 'hex');
assert.notStrictEqual(bad_dh.verifyError, 0);
  }
}

/////////////////////

export const dh_constructor_test = {
  test(ctrl, env, ctx) {
    const DiffieHellmanGroup = crypto.DiffieHellmanGroup;
    const dhg = DiffieHellmanGroup('modp14');
    assert.ok(dhg instanceof DiffieHellmanGroup, 'DiffieHellmanGroup is expected ' +
                                                 'to return a new instance when ' +
                                                 'called without `new`');

    const p1 = dhg.getPrime('buffer');
    const DiffieHellman = crypto.DiffieHellman;
    const dh = DiffieHellman(p1, 'buffer');
    assert.ok(dh instanceof DiffieHellman, 'DiffieHellman is expected to return a ' +
                                           'new instance when called without `new`');
  }
}
////////////////////

// This test will fail if boringssl runs in FIPS mode and succeed otherwise; disable it for now.

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
      function() {
        crypto.getDiffieHellman('modp14').setPrivateKey('');
      },
      new RegExp('^TypeError: crypto\\.getDiffieHellman\\(\\.\\.\\.\\)\\.' +
      'setPrivateKey is not a function$'),
      'crypto.getDiffieHellman(\'modp14\').setPrivateKey(\'\') ' +
      'failed to throw the expected error.'
    );
    assert.throws(
      function() {
        crypto.getDiffieHellman('modp14').setPublicKey('');
      },
      new RegExp('^TypeError: crypto\\.getDiffieHellman\\(\\.\\.\\.\\)\\.' +
      'setPublicKey is not a function$'),
      'crypto.getDiffieHellman(\'modp14\').setPublicKey(\'\') ' +
      'failed to throw the expected error.'
    );
  }
}

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
  }
}

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
      crypto.getDiffieHellman('modp14').getPrime());

    p.setPublicKey(apub, 'hex');
    p.setPrivateKey(apriv, 'hex');

    assert.strictEqual(
      p.computeSecret(bpub, 'hex', 'hex').toString('hex'),
      secret
    );
  }
}

assert.throws(() => crypto.diffieHellman(), {
  name: 'TypeError',
  code: 'ERR_INVALID_ARG_TYPE',
  message: 'The "options" argument must be of type object. Received undefined'
});

assert.throws(() => crypto.diffieHellman(null), {
  name: 'TypeError',
  code: 'ERR_INVALID_ARG_TYPE',
  message: 'The "options" argument must be of type object. Received null'
});

assert.throws(() => crypto.diffieHellman([]), {
  name: 'TypeError',
  code: 'ERR_INVALID_ARG_TYPE',
  message:
    'The "options" argument must be of type object. ' +
    'Received an instance of Array',
});

function test({ publicKey: alicePublicKey, privateKey: alicePrivateKey },
              { publicKey: bobPublicKey, privateKey: bobPrivateKey },
              expectedValue) {
  const buf1 = crypto.diffieHellman({
    privateKey: alicePrivateKey,
    publicKey: bobPublicKey
  });
  const buf2 = crypto.diffieHellman({
    privateKey: bobPrivateKey,
    publicKey: alicePublicKey
  });
  assert.deepStrictEqual(buf1, buf2);

  if (expectedValue !== undefined)
    assert.deepStrictEqual(buf1, expectedValue);
}

export const dh_blah = {
  async test(ctrl, env, ctx) {
    const aliceKeyPair = await crypto.subtle.generateKey(
      {
        name: "ECDH",
        namedCurve: "P-384",
      },
      true, ["deriveBits", "deriveKey"]
    );
    const alicePrivateKey = crypto.KeyObject.from(aliceKeyPair.privateKey);
    const alicePublicKey = crypto.KeyObject.from(aliceKeyPair.publicKey);
    const bobKeyPair = await crypto.subtle.generateKey(
      {
        name: "ECDH",
        namedCurve: "P-384",
      },
      true, ["deriveBits", "deriveKey"]
    );
    const bobPrivateKey = crypto.KeyObject.from(bobKeyPair.privateKey);
    const bobPublicKey = crypto.KeyObject.from(bobKeyPair.publicKey);

/* const alicePrivateKey = crypto.createPrivateKey({
  key: '-----BEGIN PRIVATE KEY-----\n' +
       'MIIBoQIBADCB1QYJKoZIhvcNAQMBMIHHAoHBAP//////////yQ/aoiFowjTExmKL\n' +
       'gNwc0SkCTgiKZ8x0Agu+pjsTmyJRSgh5jjQE3e+VGbPNOkMbMCsKbfJfFDdP4TVt\n' +
       'bVHCReSFtXZiXn7G9ExC6aY37WsL/1y29Aa37e44a/taiZ+lrp8kEXxLH+ZJKGZR\n' +
       '7ORbPcIAfLihY78FmNpINhxV05ppFj+o/STPX4NlXSPco62WHGLzViCFUrue1SkH\n' +
       'cJaWbWcMNU5KvJgE8XRsCMojcyf//////////wIBAgSBwwKBwEh82IAVnYNf0Kjb\n' +
       'qYSImDFyg9sH6CJ0GzRK05e6hM3dOSClFYi4kbA7Pr7zyfdn2SH6wSlNS14Jyrtt\n' +
       'HePrRSeYl1T+tk0AfrvaLmyM56F+9B3jwt/nzqr5YxmfVdXb2aQV53VS/mm3pB2H\n' +
       'iIt9FmvFaaOVe2DupqSr6xzbf/zyON+WF5B5HNVOWXswgpgdUsCyygs98hKy/Xje\n' +
       'TGzJUoWInW39t0YgMXenJrkS0m6wol8Rhxx81AGgELNV7EHZqg==\n' +
       '-----END PRIVATE KEY-----',
  format: 'pem'
});

const alicePublicKey = crypto.createPublicKey({
  key: '-----BEGIN PUBLIC KEY-----\n' +
       'MIIBnzCB1QYJKoZIhvcNAQMBMIHHAoHBAP//////////yQ/aoiFowjTExmKLgNwc\n' +
       '0SkCTgiKZ8x0Agu+pjsTmyJRSgh5jjQE3e+VGbPNOkMbMCsKbfJfFDdP4TVtbVHC\n' +
       'ReSFtXZiXn7G9ExC6aY37WsL/1y29Aa37e44a/taiZ+lrp8kEXxLH+ZJKGZR7ORb\n' +
       'PcIAfLihY78FmNpINhxV05ppFj+o/STPX4NlXSPco62WHGLzViCFUrue1SkHcJaW\n' +
       'bWcMNU5KvJgE8XRsCMojcyf//////////wIBAgOBxAACgcBR7+iL5qx7aOb9K+aZ\n' +
       'y2oLt7ST33sDKT+nxpag6cWDDWzPBKFDCJ8fr0v7yW453px8N4qi4R7SYYxFBaYN\n' +
       'Y3JvgDg1ct2JC9sxSuUOLqSFn3hpmAjW7cS0kExIVGfdLlYtIqbhhuo45cTEbVIM\n' +
       'rDEz8mjIlnvbWpKB9+uYmbjfVoc3leFvUBqfG2In2m23Md1swsPxr3n7g68H66JX\n' +
       'iBJKZLQMqNdbY14G9rdKmhhTJrQjC+i7Q/wI8JPhOFzHIGA=\n' +
       '-----END PUBLIC KEY-----',
  format: 'pem'
});

const bobPrivateKey = crypto.createPrivateKey({
  key: '-----BEGIN PRIVATE KEY-----' +
       'MIIBoQIBADCB1QYJKoZIhvcNAQMBMIHHAoHBAP//////////yQ/aoiFowjTExmKL' +
       'gNwc0SkCTgiKZ8x0Agu+pjsTmyJRSgh5jjQE3e+VGbPNOkMbMCsKbfJfFDdP4TVt' +
       'bVHCReSFtXZiXn7G9ExC6aY37WsL/1y29Aa37e44a/taiZ+lrp8kEXxLH+ZJKGZR' +
       '7ORbPcIAfLihY78FmNpINhxV05ppFj+o/STPX4NlXSPco62WHGLzViCFUrue1SkH' +
       'cJaWbWcMNU5KvJgE8XRsCMojcyf//////////wIBAgSBwwKBwHxnT7Zw2Ehh1vyw' +
       'eolzQFHQzyuT0y+3BF+FxK2Ox7VPguTp57wQfGHbORJ2cwCdLx2mFM7gk4tZ6COS' +
       'E3Vta85a/PuhKXNLRdP79JgLnNtVtKXB+ePDS5C2GgXH1RHvqEdJh7JYnMy7Zj4P' +
       'GagGtIy3dV5f4FA0B/2C97jQ1pO16ah8gSLQRKsNpTCw2rqsZusE0rK6RaYAef7H' +
       'y/0tmLIsHxLIn+WK9CANqMbCWoP4I178BQaqhiOBkNyNZ0ndqA==' +
       '-----END PRIVATE KEY-----',
  format: 'pem'
});

const bobPublicKey = crypto.createPublicKey({
  key: '-----BEGIN PUBLIC KEY-----\n' +
       'MIIBoDCB1QYJKoZIhvcNAQMBMIHHAoHBAP//////////yQ/aoiFowjTExmKLgNwc\n' +
       '0SkCTgiKZ8x0Agu+pjsTmyJRSgh5jjQE3e+VGbPNOkMbMCsKbfJfFDdP4TVtbVHC\n' +
       'ReSFtXZiXn7G9ExC6aY37WsL/1y29Aa37e44a/taiZ+lrp8kEXxLH+ZJKGZR7ORb\n' +
       'PcIAfLihY78FmNpINhxV05ppFj+o/STPX4NlXSPco62WHGLzViCFUrue1SkHcJaW\n' +
       'bWcMNU5KvJgE8XRsCMojcyf//////////wIBAgOBxQACgcEAi26oq8z/GNSBm3zi\n' +
       'gNt7SA7cArUBbTxINa9iLYWp6bxrvCKwDQwISN36/QUw8nUAe8aRyMt0oYn+y6vW\n' +
       'Pw5OlO+TLrUelMVFaADEzoYomH0zVGb0sW4aBN8haC0mbrPt9QshgCvjr1hEPEna\n' +
       'QFKfjzNaJRNMFFd4f2Dn8MSB4yu1xpA1T2i0JSk24vS2H55jx24xhUYtfhT2LJgK\n' +
       'JvnaODey/xtY4Kql10ZKf43Lw6gdQC3G8opC9OxVxt9oNR7Z\n' +
       '-----END PUBLIC KEY-----',
  format: 'pem'
}); */

assert.throws(() => crypto.diffieHellman({ privateKey: alicePrivateKey }), {
  name: 'TypeError',
  code: 'ERR_INVALID_ARG_VALUE',
  message: "The property 'options.publicKey' is invalid. Received undefined"
});

assert.throws(() => crypto.diffieHellman({ publicKey: alicePublicKey }), {
  name: 'TypeError',
  code: 'ERR_INVALID_ARG_VALUE',
  message: "The property 'options.privateKey' is invalid. Received undefined"
});

const privateKey = Buffer.from(
  '487CD880159D835FD0A8DBA9848898317283DB07E822741B344AD397BA84CDDD3920A51588' +
  'B891B03B3EBEF3C9F767D921FAC1294D4B5E09CABB6D1DE3EB4527989754FEB64D007EBBDA' +
  '2E6C8CE7A17EF41DE3C2DFE7CEAAF963199F55D5DBD9A415E77552FE69B7A41D87888B7D16' +
  '6BC569A3957B60EEA6A4ABEB1CDB7FFCF238DF961790791CD54E597B3082981D52C0B2CA0B' +
  '3DF212B2FD78DE4C6CC95285889D6DFDB746203177A726B912D26EB0A25F11871C7CD401A0' +
  '10B355EC41D9AA', 'hex');
const publicKey = Buffer.from(
  '8b6ea8abccff18d4819b7ce280db7b480edc02b5016d3c4835af622d85a9e9bc6bbc22b00d' +
  '0c0848ddfafd0530f275007bc691c8cb74a189fecbabd63f0e4e94ef932eb51e94c5456800' +
  'c4ce8628987d335466f4b16e1a04df21682d266eb3edf50b21802be3af58443c49da40529f' +
  '8f335a25134c1457787f60e7f0c481e32bb5c690354f68b4252936e2f4b61f9e63c76e3185' +
  '462d7e14f62c980a26f9da3837b2ff1b58e0aaa5d7464a7f8dcbc3a81d402dc6f28a42f4ec' +
  '55c6df68351ed9', 'hex');

const group = crypto.getDiffieHellman('modp14');
const dh = crypto.createDiffieHellman(group.getPrime(), group.getGenerator());
dh.setPrivateKey(privateKey);

// Test simple Diffie-Hellman, no curves involved.
  test({ publicKey: alicePublicKey, privateKey: alicePrivateKey },
       { publicKey: bobPublicKey, privateKey: bobPrivateKey });
/*test({ publicKey: alicePublicKey, privateKey: alicePrivateKey },
     { publicKey: bobPublicKey, privateKey: bobPrivateKey },
     dh.computeSecret(publicKey));

test(crypto.generateKeyPairSync('dh', { group: 'modp5' }),
     crypto.generateKeyPairSync('dh', { group: 'modp5' }));

test(crypto.generateKeyPairSync('dh', { group: 'modp5' }),
     crypto.generateKeyPairSync('dh', { prime: group.getPrime() }));*/

const list = [
  // Same generator, but different primes.
  [{ group: 'modp14' }, { group: 'modp18' }],
  // Same primes, but different generator.
  [{ group: 'modp14' }, { prime: group.getPrime(), generator: 5 }],
  // Same generator, but different primes.
  [{ primeLength: 1024 }, { primeLength: 1024 }]
];

/* for (const [params1, params2] of list) {
  assert.throws(() => {
    test(crypto.generateKeyPairSync('dh', params1),
         crypto.generateKeyPairSync('dh', params2));
  }, {
    name: 'Error',
    code: 'ERR_OSSL_EVP_DIFFERENT_PARAMETERS'
  });
} */

  }
}

/* {
  const privateKey = crypto.createPrivateKey({
    key: '-----BEGIN PRIVATE KEY-----\n' +
         'MIIBoQIBADCB1QYJKoZIhvcNAQMBMIHHAoHBAP//////////yQ/aoiFowjTExmKL\n' +
         'gNwc0SkCTgiKZ8x0Agu+pjsTmyJRSgh5jjQE3e+VGbPNOkMbMCsKbfJfFDdP4TVt\n' +
         'bVHCReSFtXZiXn7G9ExC6aY37WsL/1y29Aa37e44a/taiZ+lrp8kEXxLH+ZJKGZR\n' +
         '7ORbPcIAfLihY78FmNpINhxV05ppFj+o/STPX4NlXSPco62WHGLzViCFUrue1SkH\n' +
         'cJaWbWcMNU5KvJgE8XRsCMojcyf//////////wIBAgSBwwKBwHu9fpiqrfJJ+tl9\n' +
         'ujFtEWv4afub6A/1/7sgishOYN3YQ+nmWQlmPpveIY34an5dG82CTrixHwUzQTMF\n' +
         'JaiCW3ax9+qk31f2jTNKrQznmKgopVKXF0FEJC6H79W/8Y0U14gsI9sHpovKhfou\n' +
         'RQD0QogW7ejSwMG8hCYibfrvMm0b5PHlwimISyEKh7VtDQ1frYN/Wr9ZbiV+FePJ\n' +
         '2j6RUKYNj1Pv+B4zdMgiLLjILAs8WUfbHciU21KSJh1izVQaUQ==\n' +
         '-----END PRIVATE KEY-----'
  });
  const publicKey = crypto.createPublicKey({
    key: '-----BEGIN PUBLIC KEY-----\n' +
         'MIIBoDCB1QYJKoZIhvcNAQMBMIHHAoHBAP//////////yQ/aoiFowjTExmKLgNwc\n' +
         '0SkCTgiKZ8x0Agu+pjsTmyJRSgh5jjQE3e+VGbPNOkMbMCsKbfJfFDdP4TVtbVHC\n' +
         'ReSFtXZiXn7G9ExC6aY37WsL/1y29Aa37e44a/taiZ+lrp8kEXxLH+ZJKGZR7ORb\n' +
         'PcIAfLihY78FmNpINhxV05ppFj+o/STPX4NlXSPco62WHGLzViCFUrue1SkHcJaW\n' +
         'bWcMNU5KvJgE8XRsCMojcyf//////////wIBAgOBxQACgcEAmG9LpD8SAA6/W7oK\n' +
         'E4MCuuQtf5E8bqtcEAfYTOOvKyCS+eiX3TtZRsvHJjUBEyeO99PR/KrGVlkSuW52\n' +
         'ZOSXUOFu1L/0tqHrvRVHo+QEq3OvZ3EAyJkdtSEUTztxuUrMOyJXHDc1OUdNSnk0\n' +
         'taGX4mP3247golVx2DS4viDYs7UtaMdx03dWaP6y5StNUZQlgCIUzL7MYpC16V5y\n' +
         'KkFrE+Kp/Z77gEjivaG6YuxVj4GPLxJYbNFVTel42oSVeKuq\n' +
         '-----END PUBLIC KEY-----',
    format: 'pem'
  });

  // This key combination will result in an unusually short secret, and should
  // not cause an assertion failure.
  const secret = crypto.diffieHellman({ publicKey, privateKey });
  assert.strictEqual(secret.toString('hex'),
                     '0099d0fa242af5db9ea7330e23937a27db041f79c581500fc7f9976' +
                     '554d59d5b9ced934778d72e19a1fefc81e9d981013198748c0b5c6c' +
                     '762985eec687dc5bec5c9367b05837daee9d0bcc29024ed7f3abba1' +
                     '2794b65a745117fb0d87bc5b1b2b68c296c3f686cc29e450e4e1239' +
                     '21f56a5733fe58aabf71f14582954059c2185d342b9b0fa10c2598a' +
                     '5426c2baee7f9a686fc1e16cd4757c852bf7225a2732250548efe28' +
                     'debc26f1acdec51efe23d20786a6f8a14d360803bbc71972e87fd3');
} */

// Test ECDH.

/* test(crypto.generateKeyPairSync('ec', { namedCurve: 'secp256k1' }),
     crypto.generateKeyPairSync('ec', { namedCurve: 'secp256k1' }));

const not256k1 = crypto.getCurves().find((c) => /^sec.*(224|384|512)/.test(c));
assert.throws(() => {
  test(crypto.generateKeyPairSync('ec', { namedCurve: 'secp256k1' }),
       crypto.generateKeyPairSync('ec', { namedCurve: not256k1 }));
}, {
  name: 'Error',
  code: 'ERR_OSSL_EVP_DIFFERENT_PARAMETERS'
});

// Test ECDH-ES.

test(crypto.generateKeyPairSync('x25519'),
     crypto.generateKeyPairSync('x25519'));

assert.throws(() => {
  test(crypto.generateKeyPairSync('x448'),
       crypto.generateKeyPairSync('x25519'));
}, {
  name: 'Error',
  code: 'ERR_CRYPTO_INCOMPATIBLE_KEY',
  message: 'Incompatible key types for Diffie-Hellman: x448 and x25519'
}); */
