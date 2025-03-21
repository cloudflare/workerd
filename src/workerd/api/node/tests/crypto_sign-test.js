import {
  createSign,
  createVerify,
  createPrivateKey,
  createPublicKey,
  sign,
  verify,
} from 'node:crypto';

import { strictEqual, throws } from 'node:assert';

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

export const ed25519SignVerifyObjects = {
  test(_, env) {
    // Sign object is not allowed with ed25519
    throws(
      () => {
        const key = createPrivateKey(env['ed25519_private.pem']);
        const signer = createSign('sha256');
        signer.update('hello world');
        const signature = signer.sign(key, 'hex');
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
