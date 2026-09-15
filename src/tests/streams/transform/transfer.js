// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// structuredClone over TransformStreams: transfer rejects TypeError in
// both implementations (transferable streams unimplemented); plain
// cloning diverges — C++ DataCloneError (the generic does-not-support-
// serialization message, NOT the RPC-only one RS/WS get), TypeScript
// yields a useless empty plain Object (defect pin; see
// readable/transfer.js for the family).

import { strictEqual, throws, match } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const structuredCloneTransform = {
  test() {
    const ts = new TransformStream();
    throws(() => structuredClone(ts, { transfer: [ts] }), {
      name: 'TypeError',
      message: 'Object is not transferable',
    });
    if (usingTsImpl) {
      const clone = structuredClone(ts);
      strictEqual(clone instanceof TransformStream, false);
      strictEqual(Object.getPrototypeOf(clone), Object.prototype);
      strictEqual(Object.keys(clone).length, 0);
    } else {
      try {
        structuredClone(ts);
        throw new Error('expected DataCloneError');
      } catch (e) {
        strictEqual(e.name, 'DataCloneError');
        match(
          e.message,
          /Could not serialize object of type "TransformStream"/
        );
      }
    }
    strictEqual(ts.readable.locked, false);
    strictEqual(ts.writable.locked, false);
  },
};
