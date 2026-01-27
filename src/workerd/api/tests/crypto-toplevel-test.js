// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, ok, notStrictEqual } from 'node:assert';

// Test that crypto.getRandomValues() works at top level (module initialization time).
// This was previously disallowed but is now supported.
// See: https://github.com/cloudflare/workerd/issues/5754
const topLevelRandomValues = new Uint8Array(16);
crypto.getRandomValues(topLevelRandomValues);

// Test that crypto.randomUUID() works at top level.
const topLevelUUID = crypto.randomUUID();

export const getRandomValuesAtTopLevel = {
  test() {
    // Verify we got some random data (not all zeros)
    const hasNonZero = topLevelRandomValues.some((byte) => byte !== 0);
    ok(
      hasNonZero,
      'getRandomValues() should have filled buffer with random data'
    );
    strictEqual(topLevelRandomValues.length, 16);
  },
};

export const randomUUIDAtTopLevel = {
  test() {
    // Verify UUID format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    const uuidRegex =
      /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
    ok(
      uuidRegex.test(topLevelUUID),
      `randomUUID() should return a valid v4 UUID, got: ${topLevelUUID}`
    );
  },
};

// Test that crypto.getRandomValues() still works inside handlers
export const getRandomValuesInHandler = {
  test() {
    const buffer = new Uint8Array(32);
    crypto.getRandomValues(buffer);
    const hasNonZero = buffer.some((byte) => byte !== 0);
    ok(hasNonZero, 'getRandomValues() should work inside handler');
  },
};

// Test that crypto.randomUUID() still works inside handlers
export const randomUUIDInHandler = {
  test() {
    const uuid = crypto.randomUUID();
    const uuidRegex =
      /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
    ok(uuidRegex.test(uuid), `randomUUID() should work inside handler`);
    // Make sure it's different from the top-level one
    notStrictEqual(uuid, topLevelUUID, 'Each call should generate a new UUID');
  },
};

// Test various TypedArray types for getRandomValues
export const getRandomValuesTypedArrays = {
  test() {
    const int8 = new Int8Array(8);
    crypto.getRandomValues(int8);
    ok(
      int8.some((v) => v !== 0),
      'Int8Array'
    );

    const uint8 = new Uint8Array(8);
    crypto.getRandomValues(uint8);
    ok(
      uint8.some((v) => v !== 0),
      'Uint8Array'
    );

    const uint8Clamped = new Uint8ClampedArray(8);
    crypto.getRandomValues(uint8Clamped);
    ok(
      uint8Clamped.some((v) => v !== 0),
      'Uint8ClampedArray'
    );

    const int16 = new Int16Array(8);
    crypto.getRandomValues(int16);
    ok(
      int16.some((v) => v !== 0),
      'Int16Array'
    );

    const uint16 = new Uint16Array(8);
    crypto.getRandomValues(uint16);
    ok(
      uint16.some((v) => v !== 0),
      'Uint16Array'
    );

    const int32 = new Int32Array(8);
    crypto.getRandomValues(int32);
    ok(
      int32.some((v) => v !== 0),
      'Int32Array'
    );

    const uint32 = new Uint32Array(8);
    crypto.getRandomValues(uint32);
    ok(
      uint32.some((v) => v !== 0),
      'Uint32Array'
    );

    const bigInt64 = new BigInt64Array(4);
    crypto.getRandomValues(bigInt64);
    ok(
      bigInt64.some((v) => v !== 0n),
      'BigInt64Array'
    );

    const bigUint64 = new BigUint64Array(4);
    crypto.getRandomValues(bigUint64);
    ok(
      bigUint64.some((v) => v !== 0n),
      'BigUint64Array'
    );
  },
};
