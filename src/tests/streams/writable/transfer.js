// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// structuredClone over WritableStreams: transfer rejects TypeError in
// both implementations (transferable streams unimplemented); plain
// cloning diverges — C++ DataCloneError (RPC-only serialization),
// TypeScript yields a useless empty plain Object (defect pin; see
// readable/transfer.js for the family).

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const structuredCloneWritable = {
  test() {
    const ws = new WritableStream({});
    throws(() => structuredClone(ws, { transfer: [ws] }), {
      name: 'TypeError',
      message: 'Object is not transferable',
    });
    if (usingTsImpl) {
      const clone = structuredClone(ws);
      strictEqual(clone instanceof WritableStream, false);
      strictEqual(Object.getPrototypeOf(clone), Object.prototype);
      strictEqual(Object.keys(clone).length, 0);
    } else {
      throws(() => structuredClone(ws), {
        name: 'DataCloneError',
        message: 'WritableStream can only be serialized for RPC.',
      });
    }
    strictEqual(ws.locked, false);
  },
};
