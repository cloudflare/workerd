// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Pins the observable behaviour of the `crypto` global and its class: getRandomValues() and
// randomUUID() for every kind of argument, inside and outside a request, `crypto.subtle`,
// `crypto.DigestStream`, and the shape of the class, the instance and the globals, as the lines
// in crypto-global-parity-expected.js.

import { strictEqual } from 'node:assert';
import { inspect } from 'node:util';
import { oldest, newest } from 'crypto-global-parity-expected.js';

function errorText(e) {
  const dom = e instanceof DOMException ? ` code=${e.code}` : '';
  return `${e?.constructor?.name} ${e?.name}: ${e?.message}${dom}`;
}

function settle(fn) {
  try {
    const result = fn();
    return `RETURNS ${describe(result)}`;
  } catch (e) {
    // Not the stack, which under TS_WEBCRYPTO begins in the bootstrap (crypto/AGENTS.md).
    return `THROWS ${errorText(e)}`;
  }
}

function describe(value) {
  if (value === undefined) return 'undefined';
  if (value === null) return 'null';
  if (typeof value === 'string') return JSON.stringify(value);
  if (typeof value !== 'object' && typeof value !== 'function') {
    return String(value);
  }
  if (ArrayBuffer.isView(value)) {
    return `${value.constructor.name}(${value.byteLength})`;
  }
  return Object.prototype.toString.call(value);
}

function descriptorText(d) {
  if (d === undefined) return 'none';
  const parts = [];
  if ('value' in d) {
    const v = d.value;
    parts.push(
      `value=${typeof v === 'function' ? `fn ${v.name}/${v.length}` : describe(v)}`
    );
    parts.push(`w=${d.writable}`);
  } else {
    parts.push(`get=${d.get ? `fn ${d.get.name}/${d.get.length}` : 'none'}`);
    parts.push(`set=${d.set ? `fn ${d.set.name}/${d.set.length}` : 'none'}`);
  }
  parts.push(`e=${d.enumerable}`, `c=${d.configurable}`);
  return parts.join(' ');
}

function keyText(key) {
  return typeof key === 'symbol' ? key.toString() : key;
}

function ownDescriptors(label, object, lines) {
  for (const key of Reflect.ownKeys(object)) {
    lines.push(
      `${label} ${keyText(key)} ${descriptorText(Object.getOwnPropertyDescriptor(object, key))}`
    );
  }
}

// The keys of `object` among `names`, in the order `object` lists them.
function orderOf(object, names) {
  return Object.getOwnPropertyNames(object)
    .filter((name) => names.includes(name))
    .join(',');
}

const kViews = [
  ['Int8Array', () => new Int8Array(8)],
  ['Uint8Array', () => new Uint8Array(8)],
  ['Uint8ClampedArray', () => new Uint8ClampedArray(8)],
  ['Int16Array', () => new Int16Array(4)],
  ['Uint16Array', () => new Uint16Array(4)],
  ['Int32Array', () => new Int32Array(2)],
  ['Uint32Array', () => new Uint32Array(2)],
  ['BigInt64Array', () => new BigInt64Array(1)],
  ['BigUint64Array', () => new BigUint64Array(1)],
  ['Float32Array', () => new Float32Array(2)],
  ['Float64Array', () => new Float64Array(1)],
  ['DataView', () => new DataView(new ArrayBuffer(8))],
  ['empty Uint8Array', () => new Uint8Array(0)],
  ['64K Uint8Array', () => new Uint8Array(65536)],
  ['64K+1 Uint8Array', () => new Uint8Array(65537)],
  ['64K+4 Uint32Array', () => new Uint32Array(16385)],
  ['64K+1 Float32Array', () => new Float32Array(16385)],
  ['offset subarray', () => new Uint8Array(16).subarray(4, 8)],
  ['shared', () => new Uint8Array(new SharedArrayBuffer(8))],
  [
    'length-tracking',
    () => new Uint8Array(new ArrayBuffer(8, { maxByteLength: 16 })),
  ],
  [
    'detached',
    () => {
      const view = new Uint8Array(8);
      structuredClone(view.buffer, { transfer: [view.buffer] });
      return view;
    },
  ],
  ['subclass', () => new (class extends Uint8Array {})(8)],
  [
    'subclass faking its tag',
    () =>
      new (class extends Uint8Array {
        get [Symbol.toStringTag]() {
          return 'Float32Array';
        }
      })(8),
  ],
  [
    'out of bounds',
    () => {
      const buffer = new ArrayBuffer(8, { maxByteLength: 16 });
      const view = new Uint8Array(buffer, 0, 8);
      buffer.resize(4);
      return view;
    },
  ],
  [
    'length-tracking grown past 64K',
    () => {
      const buffer = new ArrayBuffer(8, { maxByteLength: 65544 });
      const view = new Uint8Array(buffer);
      buffer.resize(65537);
      return view;
    },
  ],
  ...(typeof Float16Array === 'function'
    ? [['Float16Array', () => new Float16Array(4)]]
    : []),
];

const kOdd = [
  ['undefined', undefined],
  ['null', null],
  ['number', 1],
  ['string', 'x'],
  ['object', {}],
  ['array', [1, 2]],
  ['ArrayBuffer', new ArrayBuffer(8)],
  ['proxy of Uint8Array', new Proxy(new Uint8Array(8), {})],
  ['symbol', Symbol('s')],
];

// What the calls do outside a request, where there is no entropy source to read.
const topLevel = [];
{
  const record = (label, fn) =>
    topLevel.push(`top level ${label} ${settle(fn)}`);
  record('getRandomValues(Uint8Array)', () =>
    crypto.getRandomValues(new Uint8Array(8))
  );
  record('getRandomValues(Float32Array)', () =>
    crypto.getRandomValues(new Float32Array(2))
  );
  record('getRandomValues(64K+1)', () =>
    crypto.getRandomValues(new Uint8Array(65537))
  );
  record('getRandomValues()', () => crypto.getRandomValues());
  record('randomUUID()', () => crypto.randomUUID());
  record('subtle', () => crypto.subtle);
}

function run() {
  const lines = [...topLevel];
  const { Crypto, SubtleCrypto } = globalThis;

  // getRandomValues() on each kind of view.
  for (const [label, make] of kViews) {
    const view = make();
    let result = settle(() => crypto.getRandomValues(view));
    if (result.startsWith('RETURNS')) {
      let same;
      try {
        same = crypto.getRandomValues(view) === view;
      } catch {
        same = 'throws';
      }
      result += ` same=${same}`;
    }
    lines.push(`getRandomValues ${label} ${result}`);
  }
  for (const [label, value] of kOdd) {
    lines.push(
      `getRandomValues ${label} ${settle(() => crypto.getRandomValues(value))}`
    );
  }
  lines.push(`getRandomValues() ${settle(() => crypto.getRandomValues())}`);
  lines.push(
    `getRandomValues 2 args ${settle(() =>
      crypto.getRandomValues(new Uint8Array(2), 'extra')
    )}`
  );

  // Only the view's own bytes are written.
  {
    const whole = new Uint8Array(4096);
    crypto.getRandomValues(whole.subarray(1024, 2048));
    const outside =
      whole.subarray(0, 1024).some((b) => b !== 0) ||
      whole.subarray(2048).some((b) => b !== 0);
    const inside = whole.subarray(1024, 2048).some((b) => b !== 0);
    lines.push(`getRandomValues writes inside=${inside} outside=${outside}`);
  }

  // Getters user code replaced on the typed arrays' prototype, which the checks must not read.
  {
    const typedArrayPrototype = Object.getPrototypeOf(Uint8Array.prototype);
    const saved = Object.getOwnPropertyDescriptors(typedArrayPrototype);
    Object.defineProperty(typedArrayPrototype, Symbol.toStringTag, {
      get() {
        return 'Float32Array';
      },
      configurable: true,
    });
    Object.defineProperty(typedArrayPrototype, 'byteLength', {
      get() {
        return 1 << 20;
      },
      configurable: true,
    });
    const tampered = [
      ['Uint8Array', () => crypto.getRandomValues(new Uint8Array(8))],
      ['Float32Array', () => crypto.getRandomValues(new Float32Array(2))],
      ['64K+1 Uint8Array', () => crypto.getRandomValues(new Uint8Array(65537))],
    ].map(([label, fn]) => {
      try {
        fn();
        return `tampered prototype ${label} returns`;
      } catch (e) {
        return `tampered prototype ${label} THROWS ${errorText(e)}`;
      }
    });
    Object.defineProperty(
      typedArrayPrototype,
      Symbol.toStringTag,
      saved[Symbol.toStringTag]
    );
    Object.defineProperty(typedArrayPrototype, 'byteLength', saved.byteLength);
    lines.push(...tampered);
  }

  // randomUUID().
  {
    const uuids = new Set();
    let wellFormed = true;
    for (let i = 0; i < 64; i++) {
      const uuid = crypto.randomUUID();
      wellFormed &&=
        /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(
          uuid
        );
      uuids.add(uuid);
    }
    lines.push(`randomUUID well formed ${wellFormed} distinct ${uuids.size}`);
    lines.push(`randomUUID with args ${typeof crypto.randomUUID(1, 2)}`);
  }

  // Receivers that are not a Crypto.
  for (const receiver of [
    ['{}', {}],
    ['undefined', undefined],
    ['globalThis', globalThis],
    ['Crypto.prototype', Crypto.prototype],
    ['Object.create(crypto)', Object.create(crypto)],
    ['a proxy of crypto', new Proxy(crypto, {})],
  ]) {
    lines.push(
      `getRandomValues on ${receiver[0]} ${settle(() =>
        Crypto.prototype.getRandomValues.call(receiver[1], new Uint8Array(4))
      )}`
    );
    lines.push(
      `randomUUID on ${receiver[0]} ${settle(() =>
        Crypto.prototype.randomUUID.call(receiver[1])
      )}`
    );
  }
  const { getRandomValues, randomUUID } = crypto;
  lines.push(
    `detached getRandomValues ${settle(() => getRandomValues(new Uint8Array(4)))}`
  );
  lines.push(`detached randomUUID ${settle(() => randomUUID())}`);
  lines.push(
    `new getRandomValues ${settle(() => new getRandomValues(new Uint8Array(4)))}`
  );
  lines.push(`new randomUUID ${settle(() => new randomUUID())}`);

  // crypto.subtle.
  const subtle = crypto.subtle;
  lines.push(`subtle same ${crypto.subtle === subtle}`);
  lines.push(`subtle instanceof ${subtle instanceof SubtleCrypto}`);
  lines.push(
    `subtle proto ${Object.getPrototypeOf(subtle) === SubtleCrypto.prototype}`
  );
  const subtleOnProto = Object.getOwnPropertyDescriptor(
    Crypto.prototype,
    'subtle'
  );
  if (subtleOnProto?.get) {
    lines.push(
      `subtle getter on {} ${settle(() => subtleOnProto.get.call({}))}`
    );
    lines.push(
      `subtle getter on proto ${settle(() => subtleOnProto.get.call(Crypto.prototype))}`
    );
  }
  lines.push(`subtle from proto ${settle(() => Crypto.prototype.subtle)}`);
  // Only the error's type: V8 words the TypeError for assigning to a getter JSG made read-only
  // differently from one for a getter with no setter (crypto/AGENTS.md).
  {
    let assigned = 'assigned';
    try {
      crypto.subtle = 1;
    } catch (e) {
      assigned = `throws ${e.constructor.name}`;
    }
    lines.push(`assign subtle ${assigned} still ${crypto.subtle === subtle}`);
  }

  // crypto.DigestStream.
  const { DigestStream } = crypto;
  lines.push(
    `DigestStream typeof ${typeof DigestStream} name=${DigestStream?.name}`
  );
  lines.push(
    `DigestStream is WritableStream ${new DigestStream('SHA-256') instanceof WritableStream}`
  );
  lines.push(
    `DigestStream from proto ${Crypto.prototype.DigestStream === DigestStream}`
  );
  lines.push(`DigestStream own ${Object.hasOwn(crypto, 'DigestStream')}`);

  // The class.
  const proto = Crypto.prototype;
  ownDescriptors('proto', proto, lines);
  ownDescriptors('static', Crypto, lines);
  lines.push(`ctor length=${Crypto.length} name=${Crypto.name}`);
  lines.push(
    `ctor [[Prototype]] is Function.prototype ${
      Object.getPrototypeOf(Crypto) === Function.prototype
    }`
  );
  lines.push(
    `proto [[Prototype]] is Object.prototype ${
      Object.getPrototypeOf(proto) === Object.prototype
    }`
  );
  lines.push(`new Crypto ${settle(() => new Crypto())}`);
  lines.push(`call Crypto ${settle(() => Crypto())}`);

  // The instance.
  ownDescriptors('instance', crypto, lines);
  lines.push(`instance proto ${Object.getPrototypeOf(crypto) === proto}`);
  lines.push(`instanceof ${crypto instanceof Crypto}`);
  lines.push(`toString ${Object.prototype.toString.call(crypto)}`);
  lines.push(`inspect ${inspect(crypto)}`);
  lines.push(`keys ${Object.keys(crypto).join(',')}`);
  lines.push(
    `own descriptors ${Object.entries(Object.getOwnPropertyDescriptors(crypto)).length}`
  );
  let cloned;
  try {
    structuredClone(crypto);
    cloned = 'RETURNS';
  } catch (e) {
    cloned = `THROWS ${errorText(e)}`;
  }
  lines.push(`structuredClone ${cloned}`);
  lines.push(`JSON ${JSON.stringify(crypto)}`);

  // The globals.
  lines.push(
    `global crypto ${descriptorText(Object.getOwnPropertyDescriptor(globalThis, 'crypto'))}`
  );
  lines.push(
    `global Crypto ${descriptorText(Object.getOwnPropertyDescriptor(globalThis, 'Crypto'))}`
  );
  lines.push(
    `global prototype Crypto ${descriptorText(
      Object.getOwnPropertyDescriptor(
        Object.getPrototypeOf(globalThis),
        'Crypto'
      )
    )}`
  );
  lines.push(
    `global order ${orderOf(globalThis, ['self', 'crypto', 'caches', 'scheduler', 'Crypto'])}`
  );
  lines.push(
    `global prototype order ${orderOf(Object.getPrototypeOf(globalThis), [
      'FormData',
      'Crypto',
      'CryptoKey',
      'CacheStorage',
    ])}`
  );

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
  test(ctrl, env) {
    // The variants run at the oldest compatibility date or with every flag; one of the flags
    // that tells them apart picks the expectations. Each service is one of `classic`, `spec`
    // (the flags that give JSG classes their Web IDL shape, as the WPT harness enables) and
    // `streams` (typescript_implemented_streams, whose DigestStream crypto.DigestStream is).
    const era = Cloudflare.compatibilityFlags['capture_async_api_throws']
      ? newest
      : oldest;
    compare(run(), era[env.variant], env.variant);
  },
};
