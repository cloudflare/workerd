// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// structuredClone over TransformStreams and their controllers: transfer
// rejects TypeError in both implementations (transferable streams
// unimplemented); plain cloning rejects DataCloneError in both, with
// the generic does-not-support-serialization message (NOT the RPC-only
// one RS/WS get in C++; see readable/transfer.js for the family).

import { strictEqual, throws } from 'node:assert';

function notSerializable(type) {
  return {
    name: 'DataCloneError',
    message: `Could not serialize object of type "${type}". This type does not support serialization.`,
  };
}

export const structuredCloneTransform = {
  test() {
    let controller;
    const ts = new TransformStream({
      start(c) {
        controller = c;
      },
    });
    throws(() => structuredClone(ts, { transfer: [ts] }), {
      name: 'TypeError',
      message: 'Object is not transferable',
    });
    throws(() => structuredClone(ts), notSerializable('TransformStream'));
    throws(
      () => structuredClone(controller),
      notSerializable('TransformStreamDefaultController')
    );
    strictEqual(ts.readable.locked, false);
    strictEqual(ts.writable.locked, false);
  },
};
