// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// structuredClone over identity streams: transfer rejects TypeError in
// both implementations (transferable streams unimplemented); plain
// cloning rejects DataCloneError in both, with the generic
// does-not-support-serialization message (see the readable suite's
// transfer.js for the family).

import { strictEqual, throws } from 'node:assert';

function notSerializable(type) {
  return {
    name: 'DataCloneError',
    message: `Could not serialize object of type "${type}". This type does not support serialization.`,
  };
}

export const structuredCloneIdentity = {
  test() {
    for (const [type, its] of [
      ['IdentityTransformStream', new IdentityTransformStream()],
      ['FixedLengthStream', new FixedLengthStream(1)],
    ]) {
      throws(() => structuredClone(its, { transfer: [its] }), {
        name: 'TypeError',
        message: 'Object is not transferable',
      });
      throws(() => structuredClone(its), notSerializable(type));
      strictEqual(its.readable.locked, false);
      strictEqual(its.writable.locked, false);
    }
  },
};
