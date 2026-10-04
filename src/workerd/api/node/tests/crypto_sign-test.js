// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import {
  createSign,
  createVerify,
  createPrivateKey,
  createPublicKey,
  createSecretKey,
  generateKeyPairSync,
  constants,
  sign,
  verify,
} from 'node:crypto';

import { ok, strictEqual, throws } from 'node:assert';

const rsaSig =
  '26bb4d9641ecec048b791322c6427f62f3f4e21f7198e9e7544c8a56af40' +
  '27bcfe1b306291188f97ced0e3ceaa5ded1ae1406ec30e46a18434e55dc6' +
  'c9f237e26b124bf7ec77e54483d7782b805aa9a74bbe0f4aa8658d7620e4' +
  'd3a305777b5dc8262c675bf23c8dc5acfe8c4fa1ca8acdd956cdcbdf1fb2' +
  'f879b37dc0ed8ec51815ff02b98eefde44ac79f886902ea69e5ed2561e9e' +
  'eb2a74ec1de677b285f8108f25e9f34cc826fbbd1ad091d231dc73eaf28a' +
  'e02f09ad84ec9a38d8a7e28bea26fb7d4db20eecd075b5b261ca7320af94' +
  'cd58ba4b26d895df4fd6f68bd4d82acdcb35557012e2f69739ce8cf4a66e' +
  'bf4550ee50f6c9cec642d66fef71495a';

export const rsaSignVerifyObjects = {
  test(_, env) {
    const key = createPrivateKey(env['rsa_private.pem']);

    throws(() => createSign(), {
      message:
        'The "algorithm" argument must be of type string. Received undefined',
    });

    const signer = createSign('sha256');
    signer.update('hello world');

    throws(() => signer.update(1), {
      message: /argument must be of type string/,
    });

    throws(() => signer.sign(), {
      message: 'No key provided to sign',
    });

    throws(() => signer.sign(env['rsa_public.pem']), {
      message: 'Failed to parse private key',
    });

    const pub = createPublicKey(env['rsa_public.pem']);
    throws(() => signer.sign(pub), {
      code: 'ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE',
    });

    const signature = signer.sign(key, 'hex');

    throws(() => signer.sign(key, 'hex'), {
      message: 'Signing context has already been finalized',
    });

    strictEqual(signature, rsaSig);

    const verify = createVerify('sha256');

    throws(() => createVerify(), {
      message:
        'The "algorithm" argument must be of type string. Received undefined',
    });

    verify.update('hello world');

    throws(() => verify.update(1), {
      message: /argument must be of type string/,
    });

    throws(() => verify.verify(), {
      message: 'No key provided to sign',
    });

    strictEqual(verify.verify(env['rsa_public.pem'], signature, 'hex'), true);

    throws(() => verify.verify(env['rsa_public.pem'], signature, 'hex'), {
      message: 'Verification context has already been finalized',
    });
  },
};

export const rsaSignVerifyOneshot = {
  test(_, env) {
    const key = createPrivateKey(env['rsa_private.pem']);
    const sig = sign('sha256', Buffer.from('hello world'), key);
    strictEqual(sig.toString('hex'), rsaSig);
    strictEqual(
      verify('sha256', Buffer.from('hello world'), env['rsa_public.pem'], sig),
      true
    );
  },
};

export const defaultDigestSignVerifyOneshot = {
  test(_, env) {
    // When the algorithm is null or undefined, Node resolves the digest from
    // the key type: SHA-256 for RSA and EC keys, and no digest for
    // digest-free key types like Ed25519.
    const data = Buffer.from('hello world');

    const keyPair = generateKeyPairSync('ec', {
      namedCurve: 'prime256v1',
      publicKeyEncoding: { type: 'spki', format: 'pem' },
      privateKeyEncoding: { type: 'pkcs8', format: 'pem' },
    });

    for (const algorithm of [undefined, null]) {
      // EC keys default to SHA-256 for both KeyObject and PEM inputs, and a
      // signature made with the default verifies under an explicit 'sha256'.
      const sig = sign(algorithm, data, createPrivateKey(keyPair.privateKey));
      strictEqual(verify(algorithm, data, keyPair.publicKey, sig), true);
      strictEqual(verify('sha256', data, keyPair.publicKey, sig), true);

      // Altered data or signature must not verify.
      strictEqual(
        verify(algorithm, Buffer.from('goodbye'), keyPair.publicKey, sig),
        false
      );
      const tampered = Buffer.from(sig);
      tampered[0] ^= 0xff;
      strictEqual(verify(algorithm, data, keyPair.publicKey, tampered), false);

      // RSA keys default to SHA-256 as well.
      const rsaDefault = sign(
        algorithm,
        data,
        createPrivateKey(env['rsa_private.pem'])
      );
      strictEqual(
        verify(algorithm, data, env['rsa_public.pem'], rsaDefault),
        true
      );
    }

    // RSA PKCS#1 v1.5 signing is deterministic, so the default-digest
    // signature is byte-identical to the explicit 'sha256' one.
    strictEqual(
      sign(null, data, createPrivateKey(env['rsa_private.pem'])).toString(
        'hex'
      ),
      rsaSig
    );
  },
};

export const ed25519SignVerifyObjects = {
  test(_, env) {
    // Sign object is not allowed with ed25519
    throws(
      () => {
        const key = createPrivateKey(env['ed25519_private.pem']);
        const signer = createSign('sha256');
        signer.update('hello world');
        const _signature = signer.sign(key, 'hex');
      },
      {
        message: 'Failed to set signature digest',
      }
    );
  },
};

export const ed25519SignVerifyOneshot = {
  test(_, env) {
    const key = createPrivateKey(env['ed25519_private.pem']);
    const sig = sign(null, Buffer.from('hello world'), key);
    strictEqual(
      verify(null, Buffer.from('hello world'), env['ed25519_public.pem'], sig),
      true
    );
  },
};

export const dsaSignVerifyObjects = {
  test(_, env) {
    const pvt = createPrivateKey(env['dsa_private.pem']);
    const pub = createPublicKey(env['dsa_public.pem']);
    const signer = createSign('sha256');
    const verifier = createVerify('sha256');
    signer.update('');
    verifier.update('');
    throws(() => signer.sign(pvt), {
      message: 'Signing with DSA keys is not currently supported',
    });
    throws(() => verifier.verify(pub, Buffer.alloc(0)), {
      message: 'Verifying with DSA keys is not currently supported',
    });
    throws(() => sign('sha256', Buffer.alloc(0), pvt), {
      message: 'Signing with DSA keys is not currently supported',
    });
    throws(() => verify('sha256', Buffer.alloc(0), pub, Buffer.alloc(0)), {
      message: 'Verifying with DSA keys is not currently supported',
    });
  },
};

export const testSignLength = {
  test() {
    // Tests that generated signatures are not overly long.
    const message = `Test message 123: ${Math.random().toString(36).substring(2, 15)}`;

    const keyPair = generateKeyPairSync('ec', {
      namedCurve: 'prime256v1',
      publicKeyEncoding: {
        type: 'spki',
        format: 'pem',
      },
      privateKeyEncoding: {
        type: 'pkcs8',
        format: 'pem',
      },
    });

    for (let n = 0; n < 1000; n++) {
      const sign = createSign('SHA256');
      sign.write(Buffer.from(message));
      sign.end();

      const sig = sign.sign(keyPair.privateKey);

      const verify = createVerify('SHA256');
      verify.write(Buffer.from(message));
      verify.end();

      // It will only verify correctly if the signature is the correct length.
      ok(verify.verify(keyPair.publicKey, sig));
    }
  },
};

// Test that Web Crypto keys (from crypto.subtle) can be used with
// Node.js crypto sign/verify one-shot functions.
export const webCryptoKeySignVerify = {
  async test() {
    const keyPair = await crypto.subtle.generateKey(
      { name: 'ECDSA', namedCurve: 'P-256' },
      true,
      ['sign', 'verify']
    );
    const data = Buffer.from('hello world');
    const sig = sign('SHA256', data, keyPair.privateKey);
    ok(sig instanceof Buffer);
    ok(sig.length > 0);
    ok(verify('SHA256', data, keyPair.publicKey, sig));
  },
};

export const webCryptoKeySignVerifyP384 = {
  async test() {
    const keyPair = await crypto.subtle.generateKey(
      { name: 'ECDSA', namedCurve: 'P-384' },
      true,
      ['sign', 'verify']
    );
    const data = Buffer.from('test data');
    const sig = sign('SHA384', data, keyPair.privateKey);
    ok(sig instanceof Buffer);
    ok(sig.length > 0);
    ok(verify('SHA384', data, keyPair.publicKey, sig));
  },
};

export const optionsKeySignVerify = {
  async test(_, env) {
    // The sign/verify options object accepts the key under options.key as a
    // KeyObject or CryptoKey; other options still come from the outer object.
    const data = Buffer.from('hello world');

    const ec = generateKeyPairSync('ec', {
      namedCurve: 'prime256v1',
      publicKeyEncoding: { type: 'spki', format: 'pem' },
      privateKeyEncoding: { type: 'pkcs8', format: 'pem' },
    });
    const ecPrivate = createPrivateKey(ec.privateKey);
    const ecPublic = createPublicKey(ec.publicKey);

    // One-shot sign + verify with options-wrapped KeyObjects.
    const ecSig = sign('sha256', data, { key: ecPrivate });
    strictEqual(verify('sha256', data, { key: ecPublic }, ecSig), true);
    strictEqual(
      verify('sha256', Buffer.from('goodbye'), { key: ecPublic }, ecSig),
      false
    );

    // Options read through the wrapper: dsaEncoding affects the signature
    // format of EC signing.
    const p1363Sig = sign('sha256', data, {
      key: ecPrivate,
      dsaEncoding: 'ieee-p1363',
    });
    strictEqual(
      verify(
        'sha256',
        data,
        { key: ecPublic, dsaEncoding: 'ieee-p1363' },
        p1363Sig
      ),
      true
    );

    // RSA through the same wrapped form, sync, streaming and callback.
    const rsaPrivate = createPrivateKey(env['rsa_private.pem']);
    const rsaPublic = createPublicKey(env['rsa_public.pem']);
    const rsaSignature = sign('sha256', data, { key: rsaPrivate });
    strictEqual(verify('sha256', data, { key: rsaPublic }, rsaSignature), true);

    const rsaSigner = createSign('sha256');
    rsaSigner.update(data);
    const rsaStreamSig = rsaSigner.sign({ key: rsaPrivate });
    const rsaVerifier = createVerify('sha256');
    rsaVerifier.update(data);
    strictEqual(rsaVerifier.verify({ key: rsaPublic }, rsaStreamSig), true);

    // RSA padding and saltLength options are read through the wrapper: a
    // PSS signature verifies only with matching options.
    const pssSignOpts = {
      key: rsaPrivate,
      padding: constants.RSA_PKCS1_PSS_PADDING,
      saltLength: 32,
    };
    const pssVerifyOpts = {
      key: rsaPublic,
      padding: constants.RSA_PKCS1_PSS_PADDING,
      saltLength: 32,
    };
    const pssSig = sign('sha256', data, pssSignOpts);
    strictEqual(verify('sha256', data, pssVerifyOpts, pssSig), true);
    strictEqual(
      verify(
        'sha256',
        data,
        {
          key: rsaPublic,
          padding: constants.RSA_PKCS1_PSS_PADDING,
          saltLength: 64,
        },
        pssSig
      ),
      false
    );
    strictEqual(verify('sha256', data, { key: rsaPublic }, pssSig), false);
    const tamperedPss = Buffer.from(pssSig);
    tamperedPss[0] ^= 0xff;
    strictEqual(verify('sha256', data, pssVerifyOpts, tamperedPss), false);

    // RSA padding and saltLength apply on the streaming Sign/Verify paths as
    // well: a streamed PSS signature verifies only with matching options.
    const pssSigner = createSign('sha256');
    pssSigner.update(data);
    const pssStreamSig = pssSigner.sign({
      key: rsaPrivate,
      padding: constants.RSA_PKCS1_PSS_PADDING,
      saltLength: 32,
    });
    const pssVerifier = createVerify('sha256');
    pssVerifier.update(data);
    strictEqual(
      pssVerifier.verify(
        {
          key: rsaPublic,
          padding: constants.RSA_PKCS1_PSS_PADDING,
          saltLength: 32,
        },
        pssStreamSig
      ),
      true
    );
    const pssVerifierMismatch = createVerify('sha256');
    pssVerifierMismatch.update(data);
    strictEqual(
      pssVerifierMismatch.verify(
        {
          key: rsaPublic,
          padding: constants.RSA_PKCS1_PSS_PADDING,
          saltLength: 64,
        },
        pssStreamSig
      ),
      false
    );
    const pssVerifierDefault = createVerify('sha256');
    pssVerifierDefault.update(data);
    strictEqual(
      pssVerifierDefault.verify({ key: rsaPublic }, pssStreamSig),
      false
    );

    // The one-shot and streaming RSA paths agree with each other in both
    // directions: a streamed PSS signature verifies one-shot and a one-shot
    // signature verifies through the streaming verifier.
    strictEqual(verify('sha256', data, pssVerifyOpts, pssStreamSig), true);
    strictEqual(
      verify('sha256', data, { key: rsaPublic }, pssStreamSig),
      false
    );
    const crossVerifier = createVerify('sha256');
    crossVerifier.update(data);
    strictEqual(crossVerifier.verify(pssVerifyOpts, pssSig), true);
    const crossVerifierDefault = createVerify('sha256');
    crossVerifierDefault.update(data);
    strictEqual(crossVerifierDefault.verify({ key: rsaPublic }, pssSig), false);

    // Streaming Sign/Verify objects accept the same wrapped form.
    const signer = createSign('sha256');
    signer.update(data);
    const streamSig = signer.sign({ key: ecPrivate });
    const verifier = createVerify('sha256');
    verifier.update(data);
    strictEqual(verifier.verify({ key: ecPublic }, streamSig), true);

    // Callback variants resolve through the same path for both EC and RSA.
    await new Promise((resolve, reject) => {
      sign('sha256', data, { key: ecPrivate }, (err, sig) => {
        if (err) reject(err);
        else {
          verify('sha256', data, { key: ecPublic }, sig, (err2, valid) => {
            if (err2) reject(err2);
            else {
              strictEqual(valid, true);
              resolve();
            }
          });
        }
      });
    });
    await new Promise((resolve, reject) => {
      sign('sha256', data, { key: rsaPrivate }, (err, sig) => {
        if (err) reject(err);
        else {
          verify('sha256', data, { key: rsaPublic }, sig, (err2, valid) => {
            if (err2) reject(err2);
            else {
              strictEqual(valid, true);
              resolve();
            }
          });
        }
      });
    });

    // options.key is read exactly once per call: a getter must not observe
    // repeated reads between validation and unwrapping.
    let signReads = 0;
    const getterSig = sign('sha256', data, {
      get key() {
        signReads++;
        return ecPrivate;
      },
    });
    strictEqual(signReads, 1);
    let verifyReads = 0;
    strictEqual(
      verify(
        'sha256',
        data,
        {
          get key() {
            verifyReads++;
            return ecPublic;
          },
        },
        getterSig
      ),
      true
    );
    strictEqual(verifyReads, 1);

    // A PEM options.key getter is read exactly once as well.
    let pemSignReads = 0;
    const pemGetterSig = sign('sha256', data, {
      get key() {
        pemSignReads++;
        return env['rsa_private.pem'];
      },
    });
    strictEqual(pemSignReads, 1);
    let pemVerifyReads = 0;
    strictEqual(
      verify(
        'sha256',
        data,
        {
          get key() {
            pemVerifyReads++;
            return env['rsa_public.pem'];
          },
        },
        pemGetterSig
      ),
      true
    );
    strictEqual(pemVerifyReads, 1);

    // Raw Buffer and ArrayBufferView material keep their raw meaning even
    // when they carry an unrelated `key` property.
    const rawPrivate = Buffer.from(env['rsa_private.pem']);
    rawPrivate.key = 'unrelated';
    const rawSig = sign('sha256', data, rawPrivate);
    const rawPublic = new Uint8Array(Buffer.from(env['rsa_public.pem']));
    rawPublic.key = { unrelated: true };
    strictEqual(verify('sha256', data, rawPublic, rawSig), true);

    // Public creation keeps its acceptance and rejection rules: a bare
    // private KeyObject derives a public key, a wrapped KeyObject under
    // options.key stays rejected, and wrapped PEM material parses.
    strictEqual(createPublicKey(rsaPrivate).type, 'public');
    throws(() => createPublicKey({ key: rsaPrivate }), {
      message: /"options.key"/,
    });
    strictEqual(
      createPrivateKey({ key: env['rsa_private.pem'] }).type,
      'private'
    );

    // A wrapped WebCrypto CryptoKey behaves like a wrapped KeyObject.
    const subtlePair = await crypto.subtle.generateKey(
      { name: 'ECDSA', namedCurve: 'P-256' },
      true,
      ['sign', 'verify']
    );
    const subtleSig = sign('sha256', data, { key: subtlePair.privateKey });
    strictEqual(
      verify('sha256', data, { key: subtlePair.publicKey }, subtleSig),
      true
    );

    // Wrapped keys still enforce the private/public key-type rules.
    throws(() => sign('sha256', data, { key: ecPublic }), {
      message: /Invalid key object type public, expected private/,
    });
    throws(() => sign('sha256', data, { key: createSecretKey(data) }), {
      code: 'ERR_INVALID_ARG_TYPE',
      message: /"options" argument must be an instance of.*'secret'\)/,
    });

    // Plain-string keys and invalid options.key values keep their existing
    // behavior.
    strictEqual(
      verify('sha256', data, { key: env['rsa_public.pem'] }, rsaSignature),
      true
    );
    throws(() => sign('sha256', data, { key: 5 }));
  },
};
