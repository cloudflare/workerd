// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { deepStrictEqual, fail, ok, strictEqual, throws } from 'node:assert';
import { createPrivateKey, generateKeyPairSync, KeyObject } from 'node:crypto';

function expectEncryptedDerRejected(algorithm, options, type) {
  const { privateKey } = generateKeyPairSync(algorithm, options);
  const plaintext = privateKey.export({ format: 'der', type });

  let exported;
  try {
    exported = privateKey.export({
      format: 'der',
      type,
      cipher: 'aes-256-cbc',
      passphrase: 'test-passphrase',
    });
  } catch (error) {
    strictEqual(error.code, 'ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS');
    strictEqual(
      error.message,
      `The selected key encoding ${type} does not support encryption.`
    );
    return;
  }

  deepStrictEqual(exported, plaintext);
  const imported = createPrivateKey({ key: exported, format: 'der', type });
  ok(privateKey.equals(imported));
  fail(`${type} DER export silently ignored cipher and passphrase`);
}

export const encrypted_der_private_key_export = {
  test() {
    expectEncryptedDerRejected('rsa', { modulusLength: 1024 }, 'pkcs1');
    expectEncryptedDerRejected('ec', { namedCurve: 'P-256' }, 'sec1');
  },
};

export const encrypted_der_rejection_precedes_passphrase_validation = {
  test() {
    const { privateKey } = generateKeyPairSync('rsa', { modulusLength: 1024 });

    throws(
      () =>
        privateKey.export({
          format: 'der',
          type: 'pkcs1',
          cipher: 'aes-256-cbc',
        }),
      {
        code: 'ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS',
        message: 'The selected key encoding pkcs1 does not support encryption.',
      }
    );
  },
};

export const key_type_rejection_precedes_encryption_rejection = {
  test() {
    const { privateKey: rsaPrivateKey } = generateKeyPairSync('rsa', {
      modulusLength: 1024,
    });
    const { privateKey: ecPrivateKey } = generateKeyPairSync('ec', {
      namedCurve: 'P-256',
    });

    throws(
      () =>
        rsaPrivateKey.export({
          format: 'der',
          type: 'sec1',
          cipher: 'aes-256-cbc',
          passphrase: 'test-passphrase',
        }),
      {
        code: 'ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS',
        message: 'The selected key encoding sec1 can only be used for EC keys.',
      }
    );
    throws(
      () =>
        ecPrivateKey.export({
          format: 'der',
          type: 'pkcs1',
          cipher: 'aes-256-cbc',
          passphrase: 'test-passphrase',
        }),
      {
        code: 'ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS',
        message:
          'The selected key encoding pkcs1 can only be used for RSA keys.',
      }
    );
  },
};

export const encrypted_der_pkcs8_control = {
  test() {
    const passphrase = 'test-passphrase';
    const { privateKey } = generateKeyPairSync('rsa', { modulusLength: 1024 });
    const encrypted = privateKey.export({
      format: 'der',
      type: 'pkcs8',
      cipher: 'aes-256-cbc',
      passphrase,
    });

    throws(() =>
      createPrivateKey({ key: encrypted, format: 'der', type: 'pkcs8' })
    );
    const imported = createPrivateKey({
      key: encrypted,
      format: 'der',
      type: 'pkcs8',
      passphrase,
    });
    ok(privateKey.equals(imported));
  },
};

export const ecdh_cryptokey_sec1_export = {
  async test() {
    const { privateKey } = await crypto.subtle.generateKey(
      { name: 'ECDH', namedCurve: 'P-256' },
      true,
      ['deriveKey', 'deriveBits']
    );
    const keyObject = KeyObject.from(privateKey);
    const plaintext = keyObject.export({ format: 'der', type: 'sec1' });
    const imported = createPrivateKey({
      key: plaintext,
      format: 'der',
      type: 'sec1',
    });
    deepStrictEqual(
      imported.export({ format: 'der', type: 'sec1' }),
      plaintext
    );

    throws(
      () =>
        keyObject.export({
          format: 'der',
          type: 'sec1',
          cipher: 'aes-256-cbc',
          passphrase: 'test-passphrase',
        }),
      {
        code: 'ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS',
        message: 'The selected key encoding sec1 does not support encryption.',
      }
    );
  },
};
