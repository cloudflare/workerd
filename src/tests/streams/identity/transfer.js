// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// structuredClone over identity streams: transfer rejects TypeError in
// both implementations (transferable streams unimplemented); plain
// cloning diverges — C++ DataCloneError (the generic does-not-support-
// serialization message), TypeScript yields a useless empty plain
// Object (defect pin; see the readable suite's transfer.js for the
// family).

import { strictEqual, throws, match } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const structuredCloneIdentity = {
  test() {
    const its = new IdentityTransformStream();
    throws(() => structuredClone(its, { transfer: [its] }), {
      name: 'TypeError',
      message: 'Object is not transferable',
    });
    if (usingTsImpl) {
      const clone = structuredClone(its);
      strictEqual(clone instanceof IdentityTransformStream, false);
      strictEqual(Object.getPrototypeOf(clone), Object.prototype);
      strictEqual(Object.keys(clone).length, 0);
    } else {
      try {
        structuredClone(its);
        throw new Error('expected DataCloneError');
      } catch (e) {
        strictEqual(e.name, 'DataCloneError');
        match(
          e.message,
          /Could not serialize object of type "IdentityTransformStream"/
        );
      }
    }
    strictEqual(its.readable.locked, false);
    strictEqual(its.writable.locked, false);
  },
};
