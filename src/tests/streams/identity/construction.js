// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructor argument handling for IdentityTransformStream and
// FixedLengthStream.
//
// The two implementations deliberately diverge on FixedLengthStream length
// validation, and those divergences are asserted per implementation below:
// - Error types: the C++ implementation reports every invalid length as
//   TypeError; the TypeScript implementation follows BigInt conversion
//   semantics (RangeError for out-of-range/non-integer numbers, TypeError
//   for wrong types).
// - Coercion: C++ accepts non-integer numbers (truncating) and numeric
//   strings; TypeScript rejects both.
// - Range: C++ rejects lengths above 2^53-1; TypeScript accepts the full
//   uint64 range.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// FixedLengthStream does not expose expectedLength directly, but the coerced
// value is observable through the highWaterMark cap: the writer's initial
// desiredSize is min(expectedLength, highWaterMark).
function coercedLength(value) {
  return new FixedLengthStream(value, {
    highWaterMark: 1000,
  }).writable.getWriter().desiredSize;
}

export const identityConstruction = {
  test() {
    // Test happy-path construction for IdentityTransformStream
    ok(new IdentityTransformStream());
    ok(new IdentityTransformStream(undefined));
    ok(new IdentityTransformStream({}));
    ok(new IdentityTransformStream({ highWaterMark: 10 }));
  },
};

export const identityBrandChecks = {
  test() {
    const its = new IdentityTransformStream();

    // Intentional divergence: The C++ implementation has IdentityTransformStream
    // inherit directly from TransformStream, while the TypeScript implementation
    // does not. The TypeScript impl follows the established convention from
    // Web Platform Apis like CompressionStream and TextEncoderStream which
    // are transforms but do not inherit from TransformStream
    if (usingTsImpl) {
      ok(!(its instanceof TransformStream));
    } else {
      ok(its instanceof TransformStream);
    }

    const fls = new FixedLengthStream(10);
    ok(fls instanceof IdentityTransformStream);

    if (usingTsImpl) {
      ok(!(fls instanceof TransformStream));
    } else {
      ok(fls instanceof TransformStream);
    }
  },
};

export const fixedLengthValidLengths = {
  test() {
    ok(new FixedLengthStream(0));
    ok(new FixedLengthStream(5));
    // Negative zero is coerced to zero.
    ok(new FixedLengthStream(-0.0));
    ok(new FixedLengthStream(Number.MAX_SAFE_INTEGER));
    // bigint lengths are accepted.
    ok(new FixedLengthStream(0n));
    ok(new FixedLengthStream(100n));
    // With a queuing strategy.
    ok(new FixedLengthStream(3, { highWaterMark: 2 }));

    // The received length is observable via the highWaterMark cap.
    strictEqual(coercedLength(5), 5);
    strictEqual(coercedLength(100n), 100);
    // A -0.0 expectedLength is normalized to +0 in both implementations
    // (C++ during uint64 coercion; TypeScript by deriving the
    // highWaterMark cap from the BigInt-coerced length). strictEqual is
    // SameValue, so a leaked negative zero would fail here.
    strictEqual(coercedLength(-0.0), 0);
  },
};

export const fixedLengthInvalidLengths = {
  test() {
    // A missing length is a TypeError in both implementations.
    throws(() => new FixedLengthStream(), TypeError);

    // Out-of-range and non-integer numeric lengths throw in both, but with
    // divergent types: C++ TypeError, TypeScript RangeError.
    const expected = usingTsImpl ? RangeError : TypeError;
    throws(() => new FixedLengthStream(-1), expected);
    throws(() => new FixedLengthStream(-1n), expected);
    throws(() => new FixedLengthStream(NaN), expected);
    throws(() => new FixedLengthStream(2n ** 64n), expected);
  },
};

export const fixedLengthCoercionDivergence = {
  test() {
    if (usingTsImpl) {
      // TypeScript follows BigInt() conversion: non-integer numbers are a
      // RangeError, and numeric strings are rejected outright. This follows
      // the guidance of the EcmaScript spec which is moving away from
      // automatic coercion.
      throws(() => new FixedLengthStream(0.5), RangeError);
      throws(() => new FixedLengthStream('10'), TypeError);
    } else {
      // C++ coerces: non-integer numbers truncate toward zero, and numeric
      // strings convert.
      strictEqual(coercedLength(0.5), 0);
      strictEqual(coercedLength(2.9), 2);
      strictEqual(coercedLength('10'), 10);
    }
  },
};

export const fixedLengthLengthsAboveMaxSafeInteger = {
  test() {
    if (usingTsImpl) {
      // TypeScript accepts the full uint64 range.
      ok(new FixedLengthStream(2 ** 53));
      ok(new FixedLengthStream(0xffff_ffff_ffff_ffffn));
    } else {
      // C++ rejects anything above 2^53-1: "FixedLengthStream requires an
      // integer expected length less than 2^53."
      throws(() => new FixedLengthStream(2 ** 53), TypeError);
      throws(() => new FixedLengthStream(0xffff_ffff_ffff_ffffn), TypeError);
    }
  },
};

export const userStrategySizeNeverInvoked = {
  async test() {
    // Both implementations consult only `highWaterMark` on the queuing
    // strategy; a user-supplied `size` callback is never invoked — with or
    // without an explicit highWaterMark — and plays no part in chunk
    // accounting. (C++ reads highWaterMark alone off the strategy bag; the
    // TypeScript implementation replaces the strategy with its own internal
    // size callback.)
    let calls = 0;
    for (const strategy of [
      { size: () => ++calls },
      { highWaterMark: 4, size: () => ++calls },
    ]) {
      for (const stream of [
        new IdentityTransformStream(strategy),
        new FixedLengthStream(3, strategy),
      ]) {
        const writer = stream.writable.getWriter();
        const reader = stream.readable.getReader();
        const write = writer.write(new Uint8Array([1, 2, 3]));
        deepStrictEqual([...(await reader.read()).value], [1, 2, 3]);
        await write;
      }
    }
    strictEqual(calls, 0);
  },
};
