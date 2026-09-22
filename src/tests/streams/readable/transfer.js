// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// structuredClone over ReadableStreams. Transferable streams (WHATWG
// §8.2) are UNIMPLEMENTED in both implementations: any
// { transfer: [stream] } rejects TypeError. Plain (non-transfer)
// cloning DIVERGES: C++ rejects DataCloneError naming the RPC-only
// serialization; TypeScript's classes carry no serialization guard, so
// V8's default object walk "succeeds" and yields a USELESS EMPTY PLAIN
// OBJECT — silent data loss, pinned as a defect. The original stream
// is untouched either way.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const structuredCloneReadable = {
  test() {
    for (const rs of [
      new ReadableStream({
        start(c) {
          c.enqueue('x');
          c.close();
        },
      }),
      new ReadableStream({ type: 'bytes' }),
    ]) {
      throws(() => structuredClone(rs, { transfer: [rs] }), {
        name: 'TypeError',
        message: 'Object is not transferable',
      });
      if (usingTsImpl) {
        const clone = structuredClone(rs);
        strictEqual(clone instanceof ReadableStream, false);
        strictEqual(Object.getPrototypeOf(clone), Object.prototype);
        strictEqual(Object.keys(clone).length, 0);
      } else {
        throws(() => structuredClone(rs), {
          name: 'DataCloneError',
          message: 'ReadableStream can only be serialized for RPC.',
        });
      }
      strictEqual(rs.locked, false); // original untouched
    }
  },
};
