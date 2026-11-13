// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Pins the observable argument handling of crypto.subtle: for every method and parameter, what a
// call with an unusual value there resolves or rejects with, which properties of the algorithm
// dictionaries are read and in what order, and the shape of the class. The C++ SubtleCrypto and
// the TypeScript one (src/per_isolate/crypto/subtle.ts, under the ts-webcrypto autogate) must
// both produce exactly the lines in crypto-subtle-parity-expected.js. The @all-autogates variant
// and crypto-subtle-parity-ts-test.wd-test run the TypeScript one.

import { strictEqual } from 'node:assert';
import { oldest, newest } from 'crypto-subtle-parity-expected.js';

const subtle = crypto.subtle;
const { SubtleCrypto } = globalThis;

function describe(value) {
  if (value === undefined) return 'undefined';
  if (value === null) return 'null';
  if (typeof value === 'symbol') return 'symbol';
  if (typeof value === 'string') return JSON.stringify(value);
  if (typeof value !== 'object' && typeof value !== 'function')
    return String(value);
  if (value instanceof CryptoKey) {
    return `CryptoKey(${value.type},${value.algorithm.name},${value.extractable},${value.usages})`;
  }
  if (value instanceof ArrayBuffer) return `ArrayBuffer(${value.byteLength})`;
  if (ArrayBuffer.isView(value)) {
    return `${value.constructor.name}(${value.byteLength})`;
  }
  if (Array.isArray(value)) return `[${value.map(describe).join(',')}]`;
  if ('privateKey' in value && 'publicKey' in value) {
    return `{privateKey:${describe(value.privateKey)},publicKey:${describe(value.publicKey)}}`;
  }
  if ('sharedKey' in value && 'ciphertext' in value) {
    return `{sharedKey:${describe(value.sharedKey)},ciphertext:${describe(value.ciphertext)}}`;
  }
  if ('kty' in value) return `JWK(${Object.keys(value).sort().join(',')})`;
  return `{${Object.keys(value).join(',')}}`;
}

function errorText(e) {
  // JSG quotes a struct by typeid(T).name(), which differs between C++ ABIs (Itanium's
  // N7workerd3api12SubtleCrypto16EncryptAlgorithmE, MSVC's
  // struct workerd::api::SubtleCrypto::EncryptAlgorithm), so the struct's name stands in.
  const text = `${e?.name}: ${e?.message}`.replace(
    /Cannot initialize (?:N7workerd3api12SubtleCrypto\d+(\w+)E|struct workerd::api::SubtleCrypto::(\w+)) /,
    (_, itanium, msvc) => `Cannot initialize SubtleCrypto::${itanium ?? msvc} `
  );
  // Messages can quote bytes, such as a JSON parse error over binary data.
  return text.replace(
    /[^\x20-\x7e]/g,
    (c) => `\\u${c.charCodeAt(0).toString(16).padStart(4, '0')}`
  );
}

// Calls fn and describes how it completes: a promise's resolution or rejection, or a
// synchronous return or throw.
async function settle(fn) {
  let result;
  try {
    result = fn();
  } catch (e) {
    return `THROWS ${errorText(e)}`;
  }
  if (!(result instanceof Promise)) return `RETURNS ${describe(result)}`;
  try {
    return `resolves ${describe(await result)}`;
  } catch (e) {
    return `rejects ${errorText(e)}`;
  }
}

// An object that records each property read, so the order the implementation reads an algorithm
// dictionary in is visible.
function recording(log, target) {
  return new Proxy(target, {
    get(t, p, r) {
      log.push(`get ${String(p)}`);
      return Reflect.get(t, p, r);
    },
    has(t, p) {
      log.push(`has ${String(p)}`);
      return Reflect.has(t, p);
    },
  });
}

const kOdd = [
  ['missing', undefined],
  ['null', null],
  ['number', 1],
  ['nan', NaN],
  ['negative', -1],
  ['fraction', 1.5],
  ['huge', 2 ** 40],
  ['string', 'x'],
  ['empty object', {}],
  ['array', []],
  ['bytes', new Uint8Array(4)],
  ['buffer', new ArrayBuffer(4)],
  ['dataview', new DataView(new ArrayBuffer(4))],
  ['boolean', true],
  ['symbol', Symbol('s')],
  ['function', () => {}],
  [
    'throwing toString',
    {
      toString: () => {
        throw new RangeError('toString');
      },
    },
  ],
];

async function makeKeys(modern) {
  const aes = await subtle.importKey(
    'raw',
    new Uint8Array(16),
    'AES-GCM',
    true,
    ['encrypt', 'decrypt', 'wrapKey', 'unwrapKey']
  );
  const aesKw = await subtle.importKey(
    'raw',
    new Uint8Array(16),
    'AES-KW',
    true,
    ['wrapKey', 'unwrapKey']
  );
  const hmac = await subtle.importKey(
    'raw',
    new Uint8Array(32),
    { name: 'HMAC', hash: 'SHA-256' },
    true,
    ['sign', 'verify']
  );
  const hkdf = await subtle.importKey(
    'raw',
    new Uint8Array(32),
    'HKDF',
    false,
    ['deriveBits', 'deriveKey']
  );
  const ec = await subtle.generateKey(
    { name: 'ECDSA', namedCurve: 'P-256' },
    true,
    ['sign', 'verify']
  );
  const nonExtractable = await subtle.importKey(
    'raw',
    new Uint8Array(16),
    'AES-GCM',
    false,
    ['encrypt']
  );
  const wrapped = await subtle.wrapKey('raw', aes, aes, {
    name: 'AES-GCM',
    iv: new Uint8Array(12),
  });
  const ciphertext = await subtle.encrypt(
    { name: 'AES-GCM', iv: new Uint8Array(12) },
    aes,
    new Uint8Array(8)
  );
  const keys = {
    aes,
    aesKw,
    hmac,
    hkdf,
    ec,
    nonExtractable,
    wrapped,
    ciphertext,
  };
  if (modern) {
    keys.kem = await subtle.generateKey('ML-KEM-768', true, [
      'encapsulateKey',
      'encapsulateBits',
      'decapsulateKey',
      'decapsulateBits',
    ]);
    keys.kemCiphertext = (
      await subtle.encapsulateBits('ML-KEM-768', keys.kem.publicKey)
    ).ciphertext;
  }
  return keys;
}

function cases(k, modern) {
  const gcm = () => ({ name: 'AES-GCM', iv: new Uint8Array(12) });
  const hkdfParams = () => ({
    name: 'HKDF',
    hash: 'SHA-256',
    salt: new Uint8Array(4),
    info: new Uint8Array(4),
  });
  const list = [
    ['encrypt', () => [gcm(), k.aes, new Uint8Array(8)]],
    ['decrypt', () => [gcm(), k.aes, k.ciphertext]],
    ['sign', () => ['HMAC', k.hmac, new Uint8Array(8)]],
    ['verify', () => ['HMAC', k.hmac, new Uint8Array(32), new Uint8Array(8)]],
    ['digest', () => ['SHA-256', new Uint8Array(8)]],
    [
      'generateKey',
      () => [{ name: 'AES-GCM', length: 128 }, true, ['encrypt']],
    ],
    [
      'deriveKey',
      () => [
        hkdfParams(),
        k.hkdf,
        { name: 'AES-GCM', length: 128 },
        true,
        ['encrypt'],
      ],
    ],
    ['deriveBits', () => [hkdfParams(), k.hkdf, 128]],
    [
      'importKey',
      () => ['raw', new Uint8Array(16), 'AES-GCM', true, ['encrypt']],
    ],
    ['exportKey', () => ['raw', k.aes]],
    ['wrapKey', () => ['raw', k.aes, k.aes, gcm()]],
    [
      'unwrapKey',
      () => ['raw', k.wrapped, k.aes, gcm(), 'AES-GCM', true, ['encrypt']],
    ],
  ];
  if (modern) {
    list.push(['getPublicKey', () => [k.ec.privateKey, ['verify']]]);
    list.push(['encapsulateBits', () => ['ML-KEM-768', k.kem.publicKey]]);
    list.push([
      'encapsulateKey',
      () => ['ML-KEM-768', k.kem.publicKey, 'AES-GCM', true, ['encrypt']],
    ]);
    list.push([
      'decapsulateBits',
      () => ['ML-KEM-768', k.kem.privateKey, k.kemCiphertext],
    ]);
    list.push([
      'decapsulateKey',
      () => [
        'ML-KEM-768',
        k.kem.privateKey,
        k.kemCiphertext,
        'AES-GCM',
        true,
        ['encrypt'],
      ],
    ]);
  }
  return list;
}

async function run(modern) {
  const lines = [];
  const k = await makeKeys(modern);

  // Every parameter, one at a time, given each unusual value, plus one argument too many.
  for (const [method, valid] of cases(k, modern)) {
    const arity = valid().length;
    lines.push(
      `${method}(valid) ${await settle(() => subtle[method](...valid()))}`
    );
    lines.push(`${method}() ${await settle(() => subtle[method]())}`);
    for (let i = 0; i < arity; i++) {
      for (const [label, value] of kOdd) {
        const args = valid();
        args[i] = value;
        if (label === 'missing' && i === arity - 1) args.length = i;
        lines.push(
          `${method} arg${i + 1}=${label} ${await settle(() => subtle[method](...args))}`
        );
      }
    }
    lines.push(
      `${method}(valid, extra) ${await settle(() => subtle[method](...valid(), 'extra'))}`
    );
  }

  // The order each dictionary's members are read in, and whether a getter runs for a member
  // that is absent.
  const dictionaries = [
    [
      'encrypt',
      (alg) => subtle.encrypt(alg, k.aes, new Uint8Array(8)),
      {
        name: 'AES-GCM',
        iv: new Uint8Array(12),
      },
    ],
    [
      'sign',
      (alg) => subtle.sign(alg, k.ec.privateKey, new Uint8Array(8)),
      {
        name: 'ECDSA',
        hash: 'SHA-256',
      },
    ],
    [
      'digest',
      (alg) => subtle.digest(alg, new Uint8Array(8)),
      { name: 'SHA-256' },
    ],
    [
      'generateKey',
      (alg) => subtle.generateKey(alg, true, ['encrypt']),
      {
        name: 'AES-GCM',
        length: 128,
      },
    ],
    [
      'importKey',
      (alg) =>
        subtle.importKey('raw', new Uint8Array(16), alg, true, ['encrypt']),
      {
        name: 'AES-GCM',
      },
    ],
    [
      'deriveBits',
      (alg) => subtle.deriveBits(alg, k.hkdf, 128),
      {
        name: 'HKDF',
        hash: 'SHA-256',
        salt: new Uint8Array(4),
        info: new Uint8Array(4),
      },
    ],
    [
      'importKey jwk',
      (jwk) => subtle.importKey('jwk', jwk, 'AES-GCM', true, ['encrypt']),
      {
        kty: 'oct',
        k: 'AAAAAAAAAAAAAAAAAAAAAA',
      },
    ],
  ];
  for (const [label, call, value] of dictionaries) {
    const log = [];
    const result = await settle(() => call(recording(log, value)));
    lines.push(`reads ${label}: ${log.join(', ')} -> ${result}`);
  }

  // Every member of every dictionary, one at a time, given each unusual value. Conversion is the
  // same whatever the variant's flags, so only the classic one runs this.
  if (!modern) {
    const jwkMembers = [
      'kty',
      'use',
      'key_ops',
      'alg',
      'ext',
      'crv',
      'x',
      'y',
      'd',
      'n',
      'e',
      'p',
      'q',
      'dp',
      'dq',
      'qi',
      'oth',
      'k',
      'pub',
      'priv',
    ];
    const members = [
      [
        'encrypt',
        [
          'name',
          'iv',
          'additionalData',
          'tagLength',
          'counter',
          'length',
          'label',
        ],
      ],
      ['sign', ['name', 'hash', 'dataLength', 'saltLength', 'context']],
      ['digest', ['name']],
      [
        'generateKey',
        [
          'name',
          'hash',
          'modulusLength',
          'publicExponent',
          'length',
          'namedCurve',
        ],
      ],
      ['importKey', ['name', 'hash', 'length', 'namedCurve', 'compressed']],
      ['deriveBits', ['name', 'salt', 'iterations', 'hash', 'public', 'info']],
      ['importKey jwk', jwkMembers],
    ];
    for (const [label, fields] of members) {
      const [, call, value] = dictionaries.find(([l]) => l === label);
      for (const field of fields) {
        for (const [odd, oddValue] of kOdd) {
          lines.push(
            `member ${label}.${field}=${odd} ${await settle(() =>
              call({ ...value, [field]: oddValue })
            )}`
          );
        }
      }
    }
    // Members of nested dictionaries, and elements of the sequences.
    const nested = [
      [
        'hash.name',
        (v) =>
          subtle.importKey(
            'raw',
            new Uint8Array(16),
            { name: 'HMAC', hash: { name: v } },
            true,
            ['sign']
          ),
      ],
      [
        'usages[0]',
        (v) =>
          subtle.importKey('raw', new Uint8Array(16), 'AES-GCM', true, [v]),
      ],
      [
        'key_ops[0]',
        (v) =>
          subtle.importKey(
            'jwk',
            { kty: 'oct', k: 'AAAAAAAAAAAAAAAAAAAAAA', key_ops: [v] },
            'AES-GCM',
            true,
            ['encrypt']
          ),
      ],
      [
        'oth[0]',
        (v) =>
          subtle.importKey(
            'jwk',
            { kty: 'oct', k: 'AAAAAAAAAAAAAAAAAAAAAA', oth: [v] },
            'AES-GCM',
            true,
            ['encrypt']
          ),
      ],
      ...['r', 'd', 't'].map((f) => [
        `oth[0].${f}`,
        (v) =>
          subtle.importKey(
            'jwk',
            { kty: 'oct', k: 'AAAAAAAAAAAAAAAAAAAAAA', oth: [{ [f]: v }] },
            'AES-GCM',
            true,
            ['encrypt']
          ),
      ]),
    ];
    for (const [label, call] of nested) {
      for (const [odd, oddValue] of kOdd) {
        lines.push(
          `member ${label}=${odd} ${await settle(() => call(oddValue))}`
        );
      }
    }
    // Arrays that are proxies, and members read from the prototype chain.
    lines.push(
      `usages proxy ${await settle(() =>
        subtle.importKey(
          'raw',
          new Uint8Array(16),
          'AES-GCM',
          true,
          new Proxy(['encrypt'], {})
        )
      )}`
    );
    lines.push(
      `key_ops proxy ${await settle(() =>
        subtle.importKey(
          'jwk',
          {
            kty: 'oct',
            k: 'AAAAAAAAAAAAAAAAAAAAAA',
            key_ops: new Proxy(['encrypt'], {}),
          },
          'AES-GCM',
          true,
          ['encrypt']
        )
      )}`
    );
    lines.push(
      `inherited member ${await settle(() =>
        subtle.encrypt(
          { __proto__: { name: 'AES-GCM', iv: new Uint8Array(12) } },
          k.aes,
          new Uint8Array(8)
        )
      )}`
    );
  }

  // A nested hash dictionary is read after the outer one's preceding members.
  {
    const log = [];
    const hash = recording(log, { name: 'SHA-256' });
    const result = await settle(() =>
      subtle.importKey(
        'raw',
        new Uint8Array(32),
        recording(log, { name: 'HMAC', hash }),
        true,
        ['sign']
      )
    );
    lines.push(`reads nested hash: ${log.join(', ')} -> ${result}`);
  }

  // Arguments are converted from first to last, before any is checked against another.
  {
    const log = [];
    const result = await settle(() =>
      subtle.importKey(
        {
          toString() {
            log.push('format');
            return 'raw';
          },
        },
        new Uint8Array(16),
        recording(log, { name: 'AES-GCM' }),
        {
          valueOf() {
            log.push('extractable');
            return true;
          },
        },
        { length: 1, 0: 'encrypt' }
      )
    );
    lines.push(`argument order: ${log.join(', ')} -> ${result}`);
  }

  // Usages given as other iterables and array-likes.
  for (const [label, usages] of [
    ['set', new Set(['encrypt'])],
    ['array-like', { length: 1, 0: 'encrypt' }],
    ['string', 'encrypt'],
    ['sparse', [, 'encrypt']], // eslint-disable-line no-sparse-arrays
    ['duplicate', ['encrypt', 'encrypt']],
    ['unknown', ['encrypt', 'bogus']],
  ]) {
    lines.push(
      `usages ${label} ${await settle(() =>
        subtle.importKey('raw', new Uint8Array(16), 'AES-GCM', true, usages)
      )}`
    );
  }

  // Key checks, in the order the implementation makes them.
  lines.push(
    `wrong algorithm ${await settle(() =>
      subtle.encrypt(
        { name: 'AES-CBC', iv: new Uint8Array(16) },
        k.aes,
        new Uint8Array(8)
      )
    )}`
  );
  lines.push(
    `wrong usage ${await settle(() =>
      subtle.sign('HMAC', k.aes, new Uint8Array(8))
    )}`
  );
  lines.push(
    `lowercase name ${await settle(() =>
      subtle.encrypt(
        { name: 'aes-gcm', iv: new Uint8Array(12) },
        k.aes,
        new Uint8Array(8)
      )
    )}`
  );
  lines.push(
    `export non-extractable ${await settle(() =>
      subtle.exportKey('raw', k.nonExtractable)
    )}`
  );
  lines.push(
    `wrap non-extractable ${await settle(() =>
      subtle.wrapKey('raw', k.nonExtractable, k.aes, {
        name: 'AES-GCM',
        iv: new Uint8Array(12),
      })
    )}`
  );
  lines.push(
    `wrap jwk ${await settle(() =>
      subtle.wrapKey('jwk', k.aes, k.aes, {
        name: 'AES-GCM',
        iv: new Uint8Array(12),
      })
    )}`
  );
  lines.push(
    `wrap aes-kw ${await settle(() => subtle.wrapKey('raw', k.aes, k.aesKw, 'AES-KW'))}`
  );
  lines.push(
    `unwrap bad jwk ${await settle(() =>
      subtle.unwrapKey(
        'jwk',
        k.wrapped,
        k.aes,
        { name: 'AES-GCM', iv: new Uint8Array(12) },
        'AES-GCM',
        true,
        ['encrypt']
      )
    )}`
  );
  lines.push(
    `import jwk as raw ${await settle(() =>
      subtle.importKey(
        'raw',
        { kty: 'oct', k: 'AAAAAAAAAAAAAAAAAAAAAA' },
        'AES-GCM',
        true,
        ['encrypt']
      )
    )}`
  );
  lines.push(
    `import jwk ext mismatch ${await settle(() =>
      subtle.importKey(
        'jwk',
        { kty: 'oct', k: 'AAAAAAAAAAAAAAAAAAAAAA', ext: false },
        'AES-GCM',
        true,
        ['encrypt']
      )
    )}`
  );
  lines.push(
    `import secret no usages ${await settle(() =>
      subtle.importKey('raw', new Uint8Array(16), 'AES-GCM', true, [])
    )}`
  );
  lines.push(
    `generate secret no usages ${await settle(() =>
      subtle.generateKey({ name: 'AES-GCM', length: 128 }, true, [])
    )}`
  );
  lines.push(
    `generate pair no private usages ${await settle(() =>
      subtle.generateKey({ name: 'ECDSA', namedCurve: 'P-256' }, true, [
        'verify',
      ])
    )}`
  );
  lines.push(
    `derive unknown type ${await settle(() =>
      subtle.deriveKey(
        {
          name: 'HKDF',
          hash: 'SHA-256',
          salt: new Uint8Array(4),
          info: new Uint8Array(4),
        },
        k.hkdf,
        'NOPE',
        true,
        ['encrypt']
      )
    )}`
  );
  lines.push(
    `derive aes no length ${await settle(() =>
      subtle.deriveKey(
        {
          name: 'HKDF',
          hash: 'SHA-256',
          salt: new Uint8Array(4),
          info: new Uint8Array(4),
        },
        k.hkdf,
        'AES-GCM',
        true,
        ['encrypt']
      )
    )}`
  );
  lines.push(
    `derive hmac ${await settle(() =>
      subtle.deriveKey(
        {
          name: 'HKDF',
          hash: 'SHA-256',
          salt: new Uint8Array(4),
          info: new Uint8Array(4),
        },
        k.hkdf,
        { name: 'HMAC', hash: 'SHA-256' },
        true,
        ['sign']
      )
    )}`
  );
  lines.push(
    `derive hmac zero length ${await settle(() =>
      subtle.deriveKey(
        {
          name: 'HKDF',
          hash: 'SHA-256',
          salt: new Uint8Array(4),
          info: new Uint8Array(4),
        },
        k.hkdf,
        { name: 'HMAC', hash: 'SHA-256', length: 0 },
        true,
        ['sign']
      )
    )}`
  );
  lines.push(
    `deriveBits null length ${await settle(() =>
      subtle.deriveBits(
        {
          name: 'HKDF',
          hash: 'SHA-256',
          salt: new Uint8Array(4),
          info: new Uint8Array(4),
        },
        k.hkdf,
        null
      )
    )}`
  );
  lines.push(
    `unknown format ${await settle(() => subtle.exportKey('bogus', k.aes))}`
  );
  lines.push(
    `modern format ${await settle(() => subtle.exportKey('raw-secret', k.aes))}`
  );
  lines.push(
    `unknown digest ${await settle(() => subtle.digest('SHA-3', new Uint8Array(1)))}`
  );
  lines.push(
    `timingSafeEqual ${await settle(() =>
      subtle.timingSafeEqual(new Uint8Array(4), new Uint8Array(4))
    )}`
  );
  lines.push(
    `timingSafeEqual lengths ${await settle(() =>
      subtle.timingSafeEqual(new Uint8Array(4), new Uint8Array(3))
    )}`
  );
  lines.push(
    `timingSafeEqual strings ${await settle(() => subtle.timingSafeEqual('a', 'a'))}`
  );
  if (modern) {
    lines.push(
      `getPublicKey secret ${await settle(() => subtle.getPublicKey(k.aes, []))}`
    );
    lines.push(
      `getPublicKey public ${await settle(() =>
        subtle.getPublicKey(k.ec.publicKey, ['verify'])
      )}`
    );
    lines.push(
      `getPublicKey bad usage ${await settle(() =>
        subtle.getPublicKey(k.ec.privateKey, ['sign'])
      )}`
    );
    for (const args of [
      ['encrypt', 'AES-GCM'],
      ['encrypt', { name: 'AES-GCM', iv: new Uint8Array(12) }],
      ['sign', 'HMAC'],
      ['digest', 'SHA-256'],
      ['generateKey', { name: 'AES-GCM', length: 128 }],
      ['importKey', 'AES-GCM'],
      [
        'deriveBits',
        {
          name: 'HKDF',
          hash: 'SHA-256',
          salt: new Uint8Array(4),
          info: new Uint8Array(4),
        },
        128,
      ],
      [
        'deriveKey',
        {
          name: 'HKDF',
          hash: 'SHA-256',
          salt: new Uint8Array(4),
          info: new Uint8Array(4),
        },
        'AES-GCM',
      ],
      ['wrapKey', 'AES-KW', 'AES-GCM'],
      ['bogus', 'AES-GCM'],
      ['encrypt', 'NOPE'],
      ['encrypt'],
      [],
    ]) {
      lines.push(
        `supports(${args.map(describe).join(',')}) ${await settle(() =>
          SubtleCrypto.supports(...args)
        )}`
      );
    }
  }

  // When each kind of outcome settles, in microtask hops after the call, and whether the error's
  // stack begins at the caller, with no frames of the implementation.
  const fromCaller = (e) =>
    !/^\s+at /.test(e.stack.split('\n')[1] ?? '') ||
    !e.stack.split('\n')[1].includes('per-isolate');
  async function hops(label, make) {
    let n = 0;
    let settled;
    let p;
    try {
      p = make();
    } catch (e) {
      lines.push(`${label} THROWS from caller ${fromCaller(e)}`);
      return;
    }
    p.then(
      () => (settled = 'resolves'),
      (e) => (settled = `rejects from caller ${fromCaller(e)}`)
    );
    while (settled === undefined && n < 20) {
      await null;
      n++;
    }
    lines.push(`${label} ${settled} after ${n} hops`);
  }
  await hops('timing digest', () =>
    subtle.digest('SHA-256', new Uint8Array(1))
  );
  await hops('timing operation error', () =>
    subtle.encrypt(
      { name: 'AES-CBC', iv: new Uint8Array(16) },
      k.aes,
      new Uint8Array(1)
    )
  );
  await hops('timing conversion error', () => subtle.encrypt());
  await hops('timing receiver error', () =>
    subtle.digest.call({}, 'SHA-256', new Uint8Array(1))
  );
  try {
    subtle.timingSafeEqual(new Uint8Array(1), new Uint8Array(2));
  } catch (e) {
    lines.push(`timingSafeEqual error from caller ${fromCaller(e)}`);
  }

  // Receivers that are not a SubtleCrypto.
  for (const method of ['encrypt', 'digest', 'importKey', 'timingSafeEqual']) {
    lines.push(
      `${method} on {} ${await settle(() =>
        SubtleCrypto.prototype[method].call({}, 'SHA-256', new Uint8Array(1))
      )}`
    );
  }

  // The class and its members.
  const proto = SubtleCrypto.prototype;
  lines.push(`proto names ${Object.getOwnPropertyNames(proto).join(',')}`);
  lines.push(
    `static names ${Object.getOwnPropertyNames(SubtleCrypto).sort().join(',')}`
  );
  for (const name of Object.getOwnPropertyNames(proto)) {
    const d = Object.getOwnPropertyDescriptor(proto, name);
    const v = d.value;
    lines.push(
      `member ${name} w=${d.writable} e=${d.enumerable} c=${d.configurable}` +
        (typeof v === 'function' ? ` length=${v.length} name=${v.name}` : '')
    );
  }
  for (const name of Object.getOwnPropertyNames(SubtleCrypto).sort()) {
    const d = Object.getOwnPropertyDescriptor(SubtleCrypto, name);
    const v = d.value;
    lines.push(
      `static ${name} w=${d.writable} e=${d.enumerable} c=${d.configurable}` +
        (typeof v === 'function' ? ` length=${v.length} name=${v.name}` : '')
    );
  }
  lines.push(`ctor length=${SubtleCrypto.length} name=${SubtleCrypto.name}`);
  lines.push(
    `ctor [[Prototype]] is Function.prototype ${
      Object.getPrototypeOf(SubtleCrypto) === Function.prototype
    }`
  );
  lines.push(
    `proto [[Prototype]] is Object.prototype ${
      Object.getPrototypeOf(proto) === Object.prototype
    }`
  );
  lines.push(
    `proto symbols ${Object.getOwnPropertySymbols(proto).map(String).join(',')}`
  );
  lines.push(`new SubtleCrypto ${await settle(() => new SubtleCrypto())}`);
  lines.push(`call SubtleCrypto ${await settle(() => SubtleCrypto())}`);
  lines.push(`instanceof ${subtle instanceof SubtleCrypto}`);
  lines.push(`same subtle ${crypto.subtle === subtle}`);
  lines.push(`proto of subtle ${Object.getPrototypeOf(subtle) === proto}`);
  lines.push(`toString ${Object.prototype.toString.call(subtle)}`);
  lines.push(`own keys of subtle ${Reflect.ownKeys(subtle).length}`);
  const subtleDesc = Object.getOwnPropertyDescriptor(
    Object.getPrototypeOf(crypto),
    'subtle'
  );
  lines.push(
    `crypto.subtle on proto ${subtleDesc !== undefined} get=${typeof subtleDesc?.get} ` +
      `set=${typeof subtleDesc?.set} e=${subtleDesc?.enumerable} c=${subtleDesc?.configurable}`
  );
  lines.push(
    `global prototype SubtleCrypto ${JSON.stringify(
      Object.getOwnPropertyDescriptor(
        Object.getPrototypeOf(globalThis),
        'SubtleCrypto'
      ),
      (key, value) => (typeof value === 'function' ? typeof value : value)
    )}`
  );
  lines.push(
    `globalThis.SubtleCrypto ${JSON.stringify(
      Object.getOwnPropertyDescriptor(globalThis, 'SubtleCrypto'),
      (key, value) => (typeof value === 'function' ? typeof value : value)
    )}`
  );
  let cloned;
  try {
    structuredClone(subtle);
    cloned = 'cloned';
  } catch (e) {
    cloned = errorText(e);
  }
  lines.push(`structuredClone ${cloned}`);

  return lines;
}

function compare(actual, expectedLines, variant) {
  // On a mismatch, print everything, so the expectations can be regenerated from the C++
  // implementation's output.
  if (actual.join('\n') !== expectedLines.join('\n')) {
    console.log(`ACTUAL BEGIN ${variant}\n${actual.join('\n')}\nACTUAL END`);
  }
  for (let i = 0; i < Math.max(actual.length, expectedLines.length); i++) {
    strictEqual(actual[i], expectedLines[i], `line ${i + 1}`);
  }
}

export default {
  async test(ctrl, env) {
    // The variants run at the oldest compatibility date or with every flag; one of the flags
    // that tells them apart picks the expectations. Each service is one of `classic`, `modern`
    // (webcrypto_modern_algorithms) and `spec` (modern, with the flags that give JSG classes
    // their Web IDL shape, as the WPT harness enables).
    const era = Cloudflare.compatibilityFlags['capture_async_api_throws']
      ? newest
      : oldest;
    compare(
      await run(env.variant !== 'classic'),
      era[env.variant],
      env.variant
    );
  },
};
