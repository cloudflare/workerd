// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Pre-flag object shape, as seen by workers predating
// workers_api_getters_setters_on_prototype (2022-01-31) and
// set_tostring_tag (2024-09-26). Legacy suite: C++ implementation only; see
// identity-cpp-legacy.wd-test.

import { ok, strictEqual } from 'node:assert';

export const legacyPropertyPlacement = {
  test() {
    const its = new IdentityTransformStream();
    for (const key of ['readable', 'writable']) {
      // An enumerable own DATA property on each instance...
      const own = Object.getOwnPropertyDescriptor(its, key);
      ok(own, `expected own ${key} property`);
      strictEqual(own.enumerable, true);
      ok('value' in own);
      strictEqual(typeof own.get, 'undefined');
      // ...and nothing anywhere on the prototype chain.
      let proto = Object.getPrototypeOf(its);
      while (proto !== null) {
        strictEqual(Object.getOwnPropertyDescriptor(proto, key), undefined);
        proto = Object.getPrototypeOf(proto);
      }
    }
    // Own data properties are trivially stable.
    strictEqual(its.readable, its.readable);
    strictEqual(its.writable, its.writable);
  },
};

export const legacyToStringTag = {
  test() {
    // Without set_tostring_tag, JSG does not brand these classes.
    strictEqual(
      Object.prototype.toString.call(new IdentityTransformStream()),
      '[object Object]'
    );
    strictEqual(
      Object.prototype.toString.call(new FixedLengthStream(0)),
      '[object Object]'
    );
  },
};
