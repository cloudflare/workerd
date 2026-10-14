// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The workerd `expectedLength` extension on a byte source: the values the
// ReadableStream constructor accepts, and the length an accepted value
// stands for, observed as the Content-Length of a fetch body built from the
// source.
//
// Both implementations convert the value as the C++ implementation
// converts any uint64_t (ledger #35). A bigint must fit in a uint64.
// Anything else goes through ToNumber (an object's valueOf runs once), must
// be finite and non-negative, and is truncated: 1.5 is 1, '5' is 5, null is
// 0. Every failure is a TypeError. C++ lets the number 2 ** 64 through its
// range check to a cast whose result is undefined, so that one value is
// asserted for TypeScript only.

import { ok, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function byteSource(expectedLength, byteCount = 0) {
  return new ReadableStream({
    type: 'bytes',
    expectedLength,
    pull(controller) {
      if (byteCount > 0) controller.enqueue(new Uint8Array(byteCount));
      controller.close();
    },
  });
}

// Posts a source that declares expectedLength and delivers byteCount bytes,
// and returns the Content-Length the subrequest carried.
async function observedContentLength(env, expectedLength, byteCount) {
  const resp = await env.SELF.fetch('http://example.org', {
    method: 'POST',
    body: byteSource(expectedLength, byteCount),
  });
  await resp.arrayBuffer();
  return resp.headers.get('observed-content-length');
}

export const byteSourceExpectedLengthAccepted = {
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
      ok(byteSource(value));
    }
    strictEqual(await observedContentLength(env, 10n, 10), '10');
  },
};

export const byteSourceExpectedLengthRejected = {
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
      throws(() => byteSource(value), TypeError);
    }
  },
};

export const byteSourceExpectedLengthCoercion = {
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

export const byteSourceExpectedLengthRange = {
  test() {
    for (const value of [2n ** 64n, 2 ** 65]) {
      throws(() => byteSource(value), TypeError);
    }
    if (usingTsImpl) throws(() => byteSource(2 ** 64), TypeError);
  },
};
