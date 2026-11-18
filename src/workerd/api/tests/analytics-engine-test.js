// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

// The binding writes each data point to the "analytics" service, as the JSON encoding of its
// AnalyticsEngineEvent message, and that service passes it on to this worker's fetch handler.
// takeDataPoint() returns the data points in the order they arrive.

const arrived = [];
const waiting = [];

function takeDataPoint() {
  if (arrived.length > 0) return Promise.resolve(arrived.shift());
  return new Promise((resolve) => waiting.push(resolve));
}

// A test case. It starts with no data points or waiters left over from a case that failed, so
// that a failure stays in its own case.
const testCase = (run) => ({
  async test(ctrl, env) {
    arrived.length = 0;
    waiting.length = 0;
    await run(env);
  },
});

const bytes = (string) => [...new TextEncoder().encode(string)];

// The message of a data point with nothing set, without its timestamp.
const EMPTY = {
  accountId: '1',
  dataset: bytes('analytics'),
  schemaVersion: '0',
  index1: [],
  ...Object.fromEntries(
    Array.from({ length: 20 }, (_, i) => [`double${i + 1}`, 0])
  ),
};

// Calls writeDataPoint(...args) and returns what it throws, or else the message it writes,
// without its timestamp, which must be the time of the call: milliseconds since the epoch,
// converted to nanoseconds in double arithmetic, so not exact above 2^53.
async function write(binding, ...args) {
  const before = Date.now();
  let returned;
  try {
    returned = binding.writeDataPoint(...args);
  } catch (e) {
    return { error: `${e.name}: ${e.message}` };
  }
  assert.strictEqual(returned, undefined);
  const { timestamp, ...message } = await takeDataPoint();
  const ms = Math.round(Number(timestamp) / 1e6);
  assert.strictEqual(String(BigInt(ms * 1e6)), timestamp);
  assert.ok(
    before <= ms && ms <= Date.now(),
    `timestamp ${ms} is not in [${before}, now]`
  );
  return { message };
}

// Asserts that writeDataPoint(...args) writes EMPTY with `fields` set.
async function assertWrites(binding, args, fields) {
  assert.deepStrictEqual(await write(binding, ...args), {
    message: { ...EMPTY, ...fields },
  });
}

async function assertThrows(binding, args, error) {
  assert.deepStrictEqual(await write(binding, ...args), { error });
}

const typeError = (field, type) =>
  `TypeError: Incorrect type for the '${field}' field on 'AnalyticsEngineEvent': ` +
  `the provided value is not of type '${type}'.`;

const NOT_AN_EVENT =
  "TypeError: Failed to execute 'writeDataPoint' on 'AnalyticsEngine': " +
  "parameter 1 is not of type 'AnalyticsEngineEvent'.";

export default {
  async fetch(request) {
    const point = await request.json();
    const resolve = waiting.shift();
    if (resolve) resolve(point);
    else arrived.push(point);
    return new Response('');
  },
};

export const shape = testCase(async (env) => {
  const binding = env.aebinding;
  const proto = Object.getPrototypeOf(binding);
  assert.strictEqual(typeof binding, 'object');
  assert.strictEqual(binding.constructor.name, 'AnalyticsEngine');
  assert.deepStrictEqual(Object.getOwnPropertyNames(binding), []);
  assert.deepStrictEqual(Object.getOwnPropertyNames(proto), [
    'writeDataPoint',
    'constructor',
  ]);
  assert.strictEqual(Object.getPrototypeOf(proto), Object.prototype);
  assert.strictEqual(binding.writeDataPoint.name, 'writeDataPoint');
  assert.strictEqual(binding.writeDataPoint.length, 0);
  assert.throws(() => binding.writeDataPoint.call({}, {}), {
    name: 'TypeError',
    message: /^Illegal invocation/,
  });
});

export const emptyDataPoints = testCase(async (env) => {
  for (const args of [[], [undefined], [null], [{}], [[]], [{}, 'extra']]) {
    await assertWrites(env.aebinding, args, {});
  }
  await assertThrows(env.aebinding, [5], NOT_AN_EVENT);
  await assertThrows(env.aebinding, ['x'], NOT_AN_EVENT);
});

export const fullDataPoint = testCase(async (env) => {
  const doubles = Array.from({ length: 20 }, (_, i) => i + 0.5);
  const blobs = Array.from({ length: 20 }, (_, i) => `b${i + 1}`);
  await assertWrites(env.aebinding, [{ indexes: ['idx'], doubles, blobs }], {
    index1: bytes('idx'),
    ...Object.fromEntries(doubles.map((d, i) => [`double${i + 1}`, d])),
    ...Object.fromEntries(blobs.map((b, i) => [`blob${i + 1}`, bytes(b)])),
  });
});

export const indexes = testCase(async (env) => {
  const ae = env.aebinding;
  const index = async (value, index1) =>
    assertWrites(ae, [{ indexes: [value] }], { index1 });
  await index(new TextEncoder().encode('ab').buffer, bytes('ab'));
  await index(new TextEncoder().encode('u8'), bytes('u8'));
  await index(new DataView(new Uint8Array([1, 2]).buffer), [1, 2]);
  await index(null, []);
  await index(undefined, []);
  await index(42, bytes('42'));
  await index({}, bytes('[object Object]'));
  await index('a'.repeat(96), bytes('a'.repeat(96)));
  await assertWrites(ae, [{ indexes: [] }], {});

  await assertThrows(ae, [{ indexes: 'abc' }], typeError('indexes', 'Array'));
  await assertThrows(ae, [{ indexes: null }], typeError('indexes', 'Array'));
});

export const blobs = testCase(async (env) => {
  const ae = env.aebinding;
  // A null or undefined blob leaves its field unset, and the next blob keeps its position.
  await assertWrites(ae, [{ blobs: [null, 'two', undefined, 'four'] }], {
    blob2: bytes('two'),
    blob4: bytes('four'),
  });
  await assertWrites(ae, [{ blobs: [1, true] }], {
    blob1: bytes('1'),
    blob2: bytes('true'),
  });
  const half = 'a'.repeat(8000);
  await assertWrites(ae, [{ blobs: [half, half] }], {
    blob1: bytes(half),
    blob2: bytes(half),
  });

  await assertThrows(ae, [{ blobs: 'abc' }], typeError('blobs', 'Array'));
  await assertThrows(ae, [{ blobs: {} }], typeError('blobs', 'Array'));
});

export const doubles = testCase(async (env) => {
  const ae = env.aebinding;
  // The JSON encoding of the message writes non-finite doubles as strings.
  await assertWrites(ae, [{ doubles: ['3', true, null, '', 1e308 * 10] }], {
    double1: 3,
    double2: 1,
    double5: 'Infinity',
  });
  await assertWrites(ae, [{ doubles: [{}, undefined, NaN] }], {
    double1: 'NaN',
    double2: 'NaN',
    double3: 'NaN',
  });

  await assertThrows(
    ae,
    [{ doubles: [1n] }],
    'TypeError: Cannot convert a BigInt value to a number'
  );
  for (const doubles of ['12', new Float64Array([1, 2]), new Set([1, 2])]) {
    await assertThrows(ae, [{ doubles }], typeError('doubles', 'Array'));
  }
});

export const order = testCase(async (env) => {
  const ae = env.aebinding;
  // The fields are read in this order.
  const read = [];
  const event = {};
  for (const [field, value] of [
    ['blobs', ['b']],
    ['doubles', [1]],
    ['indexes', ['i']],
  ]) {
    Object.defineProperty(event, field, {
      enumerable: true,
      get() {
        read.push(field);
        return value;
      },
    });
  }
  await assertWrites(ae, [event], {
    index1: bytes('i'),
    double1: 1,
    blob1: bytes('b'),
  });
  assert.deepStrictEqual(read, ['indexes', 'doubles', 'blobs']);
});

// A data point over a limit throws from writeDataPoint(), and is not written.
export const limits = testCase(async (env) => {
  const ae = env.aebinding;
  const indexTooLong =
    'TypeError: writeDataPoint(): Size of indexes[0] exceeds 96 bytes).';
  await assertThrows(ae, [{ indexes: ['a'.repeat(97)] }], indexTooLong);
  // The limit is in UTF-8 bytes.
  await assertThrows(ae, [{ indexes: ['é'.repeat(48) + 'a'] }], indexTooLong);
  await assertThrows(
    ae,
    [{ indexes: ['a', 'b'] }],
    'TypeError: writeDataPoint(): Maximum of 1 indexes supported.'
  );

  const blobsTooMany =
    'TypeError: writeDataPoint(): Maximum of 20 blobs supported.';
  await assertThrows(ae, [{ blobs: Array(21).fill('x') }], blobsTooMany);
  // Null and undefined blobs count.
  await assertThrows(ae, [{ blobs: Array(21).fill(null) }], blobsTooMany);
  const half = 'a'.repeat(8000);
  await assertThrows(
    ae,
    [{ blobs: [half, half + 'a'] }],
    'TypeError: writeDataPoint(): Cumulative size of blobs exceeds 16000 bytes).'
  );

  await assertThrows(
    ae,
    [{ doubles: Array(21).fill(1) }],
    'TypeError: writeDataPoint(): Maximum of 20 doubles supported.'
  );

  // Every field is read before any limit is checked.
  await assertThrows(
    ae,
    [
      {
        indexes: ['a', 'b'],
        get blobs() {
          throw new Error('blobs getter');
        },
      },
    ],
    'Error: blobs getter'
  );
  await assertThrows(
    ae,
    [{ indexes: ['a', 'b'], doubles: 'x' }],
    typeError('doubles', 'Array')
  );

  // The limits are checked for the indexes, then the blobs, then the doubles.
  const tooMany = Array(21).fill(1);
  await assertThrows(
    ae,
    [{ indexes: ['a', 'b'], blobs: tooMany, doubles: tooMany }],
    'TypeError: writeDataPoint(): Maximum of 1 indexes supported.'
  );
  await assertThrows(
    ae,
    [{ blobs: tooMany, doubles: tooMany }],
    'TypeError: writeDataPoint(): Maximum of 20 blobs supported.'
  );

  // None of the data points above was written: the next message is this one.
  await assertWrites(ae, [{ doubles: [7] }], { double1: 7 });
});

// A Durable Object's data point is written once the output gate opens, but one that cannot be
// written still throws from writeDataPoint().
export class Writer extends DurableObject {
  async write(event) {
    // An unawaited write holds the output gate shut until it is confirmed.
    this.ctx.storage.put('key', 'value');
    try {
      this.env.aebinding.writeDataPoint(event);
    } catch (e) {
      return `${e.name}: ${e.message}`;
    }
    return 'ok';
  }
}

export const durableObject = testCase(async (env) => {
  const writer = env.writer.get(env.writer.idFromName('writer'));
  assert.strictEqual(
    await writer.write({ doubles: Array(21).fill(1) }),
    'TypeError: writeDataPoint(): Maximum of 20 doubles supported.'
  );
  assert.strictEqual(await writer.write({ doubles: [7] }), 'ok');
  const { timestamp, ...message } = await takeDataPoint();
  assert.deepStrictEqual(message, { ...EMPTY, double1: 7 });
});
