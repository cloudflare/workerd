// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import {
  createCipheriv,
  createDecipheriv,
  randomBytes,
  createSecretKey,
  publicDecrypt,
  publicEncrypt,
  privateDecrypt,
  privateEncrypt,
  createPublicKey,
  createPrivateKey,
} from 'node:crypto';

import { strictEqual, deepStrictEqual, throws } from 'node:assert';

const tests = [
  { name: 'aes-128-cbc', size: 16, iv: 16 },
  { name: 'aes-192-cbc', size: 24, iv: 16 },
  { name: 'aes-256-cbc', size: 32, iv: 16 },
  { name: 'aes-128-ctr', size: 16, iv: 16 },
  { name: 'aes-192-ctr', size: 24, iv: 16 },
  { name: 'aes-256-ctr', size: 32, iv: 16 },
  { name: 'aes-128-ecb', size: 16, iv: 0 },
  { name: 'aes-192-ecb', size: 24, iv: 0 },
  { name: 'aes-256-ecb', size: 32, iv: 0 },
  { name: 'aes-128-ofb', size: 16, iv: 16 },
  { name: 'aes-192-ofb', size: 24, iv: 16 },
  { name: 'aes-256-ofb', size: 32, iv: 16 },
];

const authTagTests = [
  { name: 'aes-128-gcm', size: 16, iv: 16 },
  { name: 'aes-192-gcm', size: 24, iv: 16 },
  { name: 'aes-256-gcm', size: 32, iv: 16 },
];

export const cipheriv = {
  async test() {
    tests.forEach((test) => {
      const key = createSecretKey(Buffer.alloc(test.size));
      const iv = Buffer.alloc(test.iv);

      const cipher = createCipheriv(test.name, key, iv);
      const decipher = createDecipheriv(test.name, key, iv);

      let data = '';
      data += decipher.update(cipher.update('Hello World', 'utf8'));
      data += decipher.update(cipher.final());
      data += decipher.final();
      strictEqual(data, 'Hello World');
    });

    // Test that the streams API works also
    await Promise.all(
      tests.map(async (test) => {
        const { promise, resolve, reject } = Promise.withResolvers();

        const key = createSecretKey(Buffer.alloc(test.size));
        const iv = Buffer.alloc(test.iv);

        const cipher = createCipheriv(test.name, key, iv);
        const decipher = createDecipheriv(test.name, key, iv);
        decipher.setEncoding('utf8');

        cipher.end('Hello World');
        cipher.on('data', (chunk) => {
          decipher.write(chunk);
        });
        cipher.on('end', () => {
          decipher.end();
        });

        cipher.on('error', reject);
        decipher.on('error', reject);

        let res = '';
        decipher.on('data', (chunk) => {
          res += chunk;
        });
        decipher.on('end', resolve);

        await promise;
        strictEqual(res, 'Hello World', test.name);
      })
    );

    authTagTests.forEach((test) => {
      const key = createSecretKey(Buffer.alloc(test.size));
      const iv = Buffer.alloc(test.iv);

      const cipher = createCipheriv(test.name, key, iv);

      let data = '';
      cipher.setAAD(Buffer.from('hello'));
      data += cipher.update('Hello World', 'utf8', 'hex');
      data += cipher.final('hex');

      const tag = cipher.getAuthTag();

      const decipher = createDecipheriv(test.name, key, iv);
      decipher.setAuthTag(tag);
      decipher.setAAD(Buffer.from('hello'));
      let res = '';
      res += decipher.update(data, 'hex');
      res += decipher.final();
      strictEqual(res, 'Hello World');
    });
  },
};

export const largeData = {
  async test() {
    const { promise, resolve, reject } = Promise.withResolvers();

    const key = createSecretKey(Buffer.alloc(16));
    const iv = Buffer.alloc(16);
    const cipher = createCipheriv('aes-128-cbc', key, iv);
    const chunks = [
      randomBytes(1024),
      randomBytes(2048),
      randomBytes(4096),
      randomBytes(8192),
      randomBytes(16304),
    ];
    chunks.forEach((chunk) => cipher.write(chunk));
    cipher.end();

    const decipher = createDecipheriv('aes-128-cbc', key, iv);
    cipher.pipe(decipher);

    const output = [];
    decipher.on('data', (chunk) => output.push(chunk));

    decipher.on('end', () => {
      const inputCombined = Buffer.concat(chunks);
      const outputCombined = Buffer.concat(output);
      deepStrictEqual(inputCombined, outputCombined);
      resolve();
    });

    decipher.on('error', reject);

    await promise;
  },
};

export const publicEncryptPrivateDecrypt = {
  test(_, env) {
    const pub = createPublicKey(env['rsa_public.pem']);
    const pvt = createPrivateKey(env['rsa_private.pem']);

    pub.oaepLabel = 'test';
    pub.oaepHash = 'sha256';
    pub.padding = 4;
    pub.encoding = 'utf8';

    pvt.oaepLabel = 'test';
    pvt.oaepHash = 'sha256';
    pvt.padding = 4;
    pvt.encoding = 'utf8';

    strictEqual(
      privateDecrypt(pvt, publicEncrypt(pub, 'hello')).toString(),
      'hello'
    );
  },
};

export const publicEncryptPrivateDecryptDer = {
  test(_, env) {
    const pub = createPublicKey(env['rsa_public.pem']);
    const pvt = createPrivateKey(env['rsa_private.pem']);

    const pubDer = {
      key: pub.export({ type: 'pkcs1', format: 'der' }),
      format: 'der',
      type: 'pkcs1',
    };

    const pvtDer = {
      key: pvt.export({ type: 'pkcs8', format: 'der' }),
      format: 'der',
      type: 'pkcs8',
    };

    strictEqual(
      privateDecrypt(pvtDer, publicEncrypt(pubDer, 'hello')).toString(),
      'hello'
    );
  },
};

export const publicEncryptPrivateDecryptPem = {
  test(_, env) {
    const pub = createPublicKey(env['rsa_public.pem']);
    const pvt = createPrivateKey(env['rsa_private.pem']);

    const pubPem = {
      key: pub.export({ type: 'pkcs1', format: 'pem' }),
      format: 'pem',
      type: 'pkcs1',
    };

    const pvtPem = {
      key: pvt.export({ type: 'pkcs8', format: 'pem' }),
      format: 'pem',
      type: 'pkcs8',
    };

    strictEqual(
      privateDecrypt(pvtPem, publicEncrypt(pubPem, 'hello')).toString(),
      'hello'
    );
  },
};

export const publicEncryptPrivateDecryptPem2 = {
  test(_, env) {
    const pub = createPublicKey(env['rsa_public.pem']);
    const pvt = createPrivateKey(env['rsa_private.pem']);

    const pubPem = pub.export({ type: 'pkcs1', format: 'pem' });
    const pvtPem = pvt.export({ type: 'pkcs8', format: 'pem' });

    strictEqual(
      privateDecrypt(pvtPem, publicEncrypt(pubPem, 'hello')).toString(),
      'hello'
    );
  },
};

export const publicEncryptPrivateDecryptPem3 = {
  test(_, env) {
    const pub = createPublicKey(env['rsa_public.pem']);
    const pvt = createPrivateKey(env['rsa_private.pem']);

    const pubPem = pub.export({ type: 'spki', format: 'pem' });
    const pvtPem = pvt.export({ type: 'pkcs8', format: 'pem' });

    strictEqual(
      privateDecrypt(pvtPem, publicEncrypt(pubPem, 'hello')).toString(),
      'hello'
    );
  },
};

export const privateEncryptPublicDecrypt = {
  test(_, env) {
    const pub = createPublicKey(env['rsa_public.pem']);
    const pvt = createPrivateKey(env['rsa_private.pem']);

    pub.oaepLabel = 'test';
    pub.oaepHash = 'sha256';
    pub.padding = 3;
    pub.encoding = 'utf8';

    pvt.oaepLabel = 'test';
    pvt.oaepHash = 'sha256';
    pvt.padding = 3;
    pvt.encoding = 'utf8';

    const input = 'a'.repeat(256);
    strictEqual(
      publicDecrypt(pub, privateEncrypt(pvt, input)).toString(),
      input
    );
  },
};

export const missingArgChecks = {
  test() {
    throws(() => publicEncrypt(), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => publicDecrypt(), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => privateEncrypt(), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => privateDecrypt(), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => publicEncrypt('key'), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => publicDecrypt('key'), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => privateEncrypt('key'), {
      code: 'ERR_MISSING_ARGS',
    });
    throws(() => privateDecrypt('key'), {
      code: 'ERR_MISSING_ARGS',
    });
  },
};
