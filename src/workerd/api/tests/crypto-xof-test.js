// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects, strictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';

const vectors = [
  [
    'cSHAKE256',
    new Uint8Array(),
    '46b9dd2b0ba88d13233b3feb743eeb243fcd52ea62b81b82b50c27646ed5762f' +
      'd75dc4ddd8c0f200cb05019d67b592f6fc821c49479ab48640292eacb3b7c4be',
  ],
  [
    'cSHAKE256',
    new Uint8Array([
      21, 110, 234, 124, 193, 76, 86, 203, 148, 219, 3, 10, 74, 157, 149, 255,
    ]),
    '1738113f5abb3ee5320ee18aa266c3617a7475dbd8ed9a985994fddd6112ad99' +
      '9ec8e2ebdfeafb96e76f6bb3a3adba43da60f00cd12496df5af3e28ae6d3de42',
  ],
  [
    'TurboSHAKE256',
    new Uint8Array(),
    '367a329dafea871c7802ec67f905ae13c57695dc2c6663c61035f59a18f8e7db' +
      '11edc0e12e91ea60eb6b32df06dd7f002fbafabb6e13ec1cc20d995547600db0',
  ],
  [
    'TurboSHAKE256',
    new Uint8Array([0]),
    '3e1712f928f8eaf1054632b2aa0a246ed8b0c378728f60bc970410155c28820e' +
      '90cc90d8a3006aa2372c5c5ea176b0682bf22bae7467ac94f74d43d39b0482e2',
  ],
];

const names = ['cSHAKE256', 'TurboSHAKE256'];
const supports = (operation, algorithm, ...args) =>
  crypto.subtle.constructor.supports(operation, algorithm, ...args);

async function checkRejectedDigest(algorithm, errorName) {
  await rejects(async () => crypto.subtle.digest(algorithm, new Uint8Array()), {
    name: errorName,
  });
  strictEqual(supports('digest', algorithm), false);
}

export const xofDigest = {
  async test(_controller, env) {
    for (const [name, input, expected] of vectors) {
      for (const outputLength of [8, 256, 512]) {
        for (const algorithm of [
          { name, outputLength },
          { name: name.toLowerCase(), outputLength },
        ]) {
          const digest = crypto.subtle.digest(algorithm, input);
          if (env.ENABLED) {
            strictEqual(
              Buffer.from(await digest).toString('hex'),
              expected.slice(0, outputLength / 4)
            );
            strictEqual(supports('digest', algorithm), true);
          } else {
            await rejects(digest, { name: 'NotSupportedError' });
          }
        }
      }
    }
    if (!env.ENABLED) {
      strictEqual(crypto.subtle.constructor.supports, undefined);
    }
  },
};

export const xofOutputLength = {
  async test(_controller, env) {
    if (!env.ENABLED) return;
    for (const name of names) {
      await checkRejectedDigest(name, 'TypeError');
      await checkRejectedDigest({ name }, 'TypeError');
      for (const outputLength of [
        undefined,
        NaN,
        Infinity,
        -Infinity,
        -1,
        2 ** 32,
        1n,
      ]) {
        await checkRejectedDigest({ name, outputLength }, 'TypeError');
      }
      for (const outputLength of [1, 7, 9, 255]) {
        await checkRejectedDigest(
          { name, outputLength },
          name === 'cSHAKE256' ? 'NotSupportedError' : 'OperationError'
        );
      }
      for (const outputLength of [8.9, '8']) {
        const algorithm = { name, outputLength };
        const digest = await crypto.subtle.digest(algorithm, new Uint8Array());
        const expected = vectors.find((vector) => vector[0] === name)[2];
        strictEqual(Buffer.from(digest).toString('hex'), expected.slice(0, 2));
        strictEqual(supports('digest', algorithm), true);
      }
      for (const outputLength of [0, -0.5, null]) {
        const algorithm = { name, outputLength };
        if (name === 'cSHAKE256') {
          const digest = await crypto.subtle.digest(
            algorithm,
            new Uint8Array()
          );
          strictEqual(digest.byteLength, 0);
          strictEqual(supports('digest', algorithm), true);
        } else {
          await checkRejectedDigest(algorithm, 'OperationError');
        }
      }
    }
  },
};

export const cshakeParameters = {
  async test(_controller, env) {
    if (!env.ENABLED) return;
    const expected = vectors[0][2].slice(0, 64);
    const backing = new Uint8Array([1, 2, 3, 4]);
    const emptyValues = [
      undefined,
      new ArrayBuffer(0),
      new Uint8Array(),
      new DataView(new ArrayBuffer(0)),
      backing.subarray(2, 2),
    ];
    for (const value of emptyValues) {
      const algorithm = {
        name: 'cSHAKE256',
        outputLength: 256,
        functionName: value,
        customization: value,
      };
      strictEqual(
        Buffer.from(
          await crypto.subtle.digest(algorithm, new Uint8Array())
        ).toString('hex'),
        expected
      );
      strictEqual(supports('digest', algorithm), true);
    }
    for (const field of ['functionName', 'customization']) {
      for (const value of [
        new Uint8Array(1),
        new ArrayBuffer(1),
        backing.subarray(1, 2),
      ]) {
        for (const outputLength of [0, 256]) {
          await checkRejectedDigest(
            { name: 'cSHAKE256', outputLength, [field]: value },
            'NotSupportedError'
          );
        }
      }
      for (const value of [null, '', [], { byteLength: 0 }]) {
        await checkRejectedDigest(
          { name: 'cSHAKE256', outputLength: 256, [field]: value },
          'TypeError'
        );
      }
    }
  },
};

export const turboshakeDomainSeparation = {
  async test(_controller, env) {
    if (!env.ENABLED) return;
    const expected = vectors[2][2];
    for (const domainSeparation of [undefined, 0x1f, 31.9, '31']) {
      const algorithm = {
        name: 'TurboSHAKE256',
        outputLength: 512,
        domainSeparation,
      };
      strictEqual(
        Buffer.from(
          await crypto.subtle.digest(algorithm, new Uint8Array())
        ).toString('hex'),
        expected
      );
      strictEqual(supports('digest', algorithm), true);
    }
    for (const domainSeparation of [1, 6, 32, 127, 127.9]) {
      await checkRejectedDigest(
        { name: 'TurboSHAKE256', outputLength: 256, domainSeparation },
        'NotSupportedError'
      );
    }
    for (const domainSeparation of [0, -0.5, null, 128, 255]) {
      await checkRejectedDigest(
        { name: 'TurboSHAKE256', outputLength: 256, domainSeparation },
        'OperationError'
      );
    }
    for (const domainSeparation of [-1, 256, NaN, Infinity, -Infinity, 1n]) {
      await checkRejectedDigest(
        { name: 'TurboSHAKE256', outputLength: 256, domainSeparation },
        'TypeError'
      );
    }
  },
};

export const xofDigestOnly = {
  async test(_controller, env) {
    const data = new Uint8Array([1, 2, 3]);
    const ec = await crypto.subtle.generateKey(
      { name: 'ECDSA', namedCurve: 'P-256' },
      false,
      ['sign']
    );
    for (const name of names) {
      const algorithm = { name, outputLength: 256 };
      for (const operation of [
        'generateKey',
        'importKey',
        'exportKey',
        'encrypt',
        'decrypt',
        'sign',
        'verify',
        'deriveKey',
        'deriveBits',
        'wrapKey',
        'unwrapKey',
        'encapsulateKey',
        'encapsulateBits',
        'decapsulateKey',
        'decapsulateBits',
        'getPublicKey',
      ]) {
        if (env.ENABLED) strictEqual(supports(operation, algorithm), false);
      }
      await rejects(
        async () => crypto.subtle.generateKey(algorithm, false, ['encrypt']),
        { name: 'NotSupportedError' }
      );
      await rejects(
        async () =>
          crypto.subtle.importKey('raw', data, algorithm, false, ['encrypt']),
        { name: 'NotSupportedError' }
      );
      for (const hash of [name, algorithm]) {
        const hmac = { name: 'HMAC', hash };
        const rsa = {
          name: 'RSA-PSS',
          hash,
          modulusLength: 2048,
          publicExponent: new Uint8Array([1, 0, 1]),
        };
        for (const keyed of [hmac, rsa]) {
          await rejects(
            async () => crypto.subtle.generateKey(keyed, false, ['sign']),
            { name: 'NotSupportedError' }
          );
          if (env.ENABLED) strictEqual(supports('generateKey', keyed), false);
        }
        await rejects(
          async () =>
            crypto.subtle.importKey('raw', data, hmac, false, ['sign']),
          { name: 'NotSupportedError' }
        );
        if (env.ENABLED) strictEqual(supports('importKey', hmac), false);
        const sign = { name: 'ECDSA', hash };
        await rejects(
          async () => crypto.subtle.sign(sign, ec.privateKey, data),
          { name: 'NotSupportedError' }
        );
        if (env.ENABLED) strictEqual(supports('sign', sign), false);
        for (const name of ['HKDF', 'PBKDF2']) {
          const key = await crypto.subtle.importKey('raw', data, name, false, [
            'deriveBits',
          ]);
          const derive = { name, hash, salt: data, info: data, iterations: 1 };
          await rejects(
            async () => crypto.subtle.deriveBits(derive, key, 256),
            { name: 'NotSupportedError' }
          );
          if (env.ENABLED)
            strictEqual(supports('deriveBits', derive, 256), false);
        }
      }
    }
  },
};

export const xofIgnoredFields = {
  async test(_controller, env) {
    for (const algorithm of [null, undefined]) {
      await rejects(
        async () => crypto.subtle.digest(algorithm, new Uint8Array()),
        {
          name: 'TypeError',
        }
      );
    }
    const cases = [
      [
        { name: 'SHA-256' },
        [
          'outputLength',
          'domainSeparation',
          'functionName',
          'customization',
          'self',
        ],
        'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
      ],
    ];
    if (env.ENABLED) {
      cases.push(
        [
          { name: 'cSHAKE256', outputLength: 256 },
          ['domainSeparation'],
          vectors[0][2].slice(0, 64),
        ],
        [
          { name: 'TurboSHAKE256', outputLength: 256 },
          ['functionName', 'customization'],
          vectors[2][2].slice(0, 64),
        ]
      );
    }
    for (const [algorithm, fields, expected] of cases) {
      for (const field of fields) {
        Object.defineProperty(algorithm, field, {
          get() {
            throw new Error(`${field} must not be read`);
          },
        });
      }
      strictEqual(
        Buffer.from(
          await crypto.subtle.digest(algorithm, new Uint8Array())
        ).toString('hex'),
        expected
      );
      if (env.ENABLED) strictEqual(supports('digest', algorithm), true);
    }
  },
};

export const xofTransferredInput = {
  async test(_controller, env) {
    if (!env.ENABLED) return;
    for (const name of names) {
      const input = new Uint8Array([1]);
      const algorithm = {
        get name() {
          if (input.byteLength) input.buffer.transfer();
          return name;
        },
        outputLength: 256,
      };
      const expected = vectors
        .find((vector) => vector[0] === name)[2]
        .slice(0, 64);
      strictEqual(
        Buffer.from(await crypto.subtle.digest(algorithm, input)).toString(
          'hex'
        ),
        expected
      );
    }
  },
};
