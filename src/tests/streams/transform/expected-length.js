// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The workerd `transformer.expectedLength` extension: the values the
// TransformStream constructor accepts, and the length an accepted value
// stands for, observed as the Content-Length of a fetch body built from the
// readable side (the mechanism roundtrip.js uses).
//
// Both implementations convert the value as the C++ implementation
// converts any uint64_t (ledger #21), as they do a byte source's
// expectedLength (the readable-byte suite's ledger #35). A bigint must fit
// in a uint64. Anything else goes through ToNumber (an object's valueOf
// runs once), must be finite and non-negative, and is truncated: 1.5 is 1,
// '5' is 5, null is 0. Every failure is a TypeError. C++ lets the number
// 2 ** 64 through its range check to a cast whose result is undefined, so
// that one value is asserted for TypeScript only.

import { ok, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function transformWithLength(expectedLength) {
  return new TransformStream({ expectedLength });
}

// Posts the readable side of a transform that declares expectedLength and is
// written byteCount bytes, and returns the Content-Length the subrequest
// carried.
async function observedContentLength(env, expectedLength, byteCount) {
  const { readable, writable } = transformWithLength(expectedLength);
  const writer = writable.getWriter();
  if (byteCount > 0) writer.write(new Uint8Array(byteCount));
  writer.close();
  const resp = await env.SELF.fetch('http://example.org', {
    method: 'POST',
    body: readable,
  });
  await resp.arrayBuffer();
  return resp.headers.get('observed-content-length');
}

export const transformExpectedLengthAccepted = {
  async test(ctrl, env) {
    // Integral lengths up to the uint64 maximum, as numbers or bigints.
    for (const value of [
      0,
      -0,
      10,
      10n,
      2 ** 53,
      2n ** 53n,
      0xffff_ffff_ffff_ffffn,
    ]) {
      ok(transformWithLength(value));
    }
    strictEqual(await observedContentLength(env, 10n, 10), '10');
  },
};

export const transformExpectedLengthRejected = {
  test() {
    for (const value of [
      -1,
      -1n,
      -0.5,
      NaN,
      Infinity,
      -Infinity,
      'abc',
      {},
      Symbol(),
    ]) {
      throws(() => transformWithLength(value), TypeError);
    }
  },
};

export const transformExpectedLengthCoercion = {
  async test(ctrl, env) {
    let valueOfCalls = 0;
    const object = {
      valueOf() {
        valueOfCalls++;
        return 3;
      },
    };
    for (const [value, length] of [
      [1.5, 1],
      ['5', 5],
      [null, 0],
      [true, 1],
      [object, 3],
    ]) {
      strictEqual(
        await observedContentLength(env, value, length),
        String(length)
      );
    }
    strictEqual(valueOfCalls, 1);
  },
};

export const transformExpectedLengthRange = {
  test() {
    for (const value of [2n ** 64n, 2 ** 65]) {
      throws(() => transformWithLength(value), TypeError);
    }
    if (usingTsImpl) throws(() => transformWithLength(2 ** 64), TypeError);
  },
};
