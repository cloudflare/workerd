// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// structuredClone over WritableStreams and the objects they hand out:
// transfer rejects TypeError in both implementations (transferable
// streams unimplemented); plain cloning rejects DataCloneError in both,
// with the stream's message diverging as for ReadableStream (see
// readable/transfer.js for the family).

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function notSerializable(type) {
  return {
    name: 'DataCloneError',
    message: `Could not serialize object of type "${type}". This type does not support serialization.`,
  };
}

export const structuredCloneWritable = {
  test() {
    let controller;
    const ws = new WritableStream({
      start(c) {
        controller = c;
      },
    });
    throws(() => structuredClone(ws, { transfer: [ws] }), {
      name: 'TypeError',
      message: 'Object is not transferable',
    });
    throws(
      () => structuredClone(ws),
      usingTsImpl
        ? notSerializable('WritableStream')
        : {
            name: 'DataCloneError',
            message: 'WritableStream can only be serialized for RPC.',
          }
    );
    strictEqual(ws.locked, false);

    throws(
      () => structuredClone(controller),
      notSerializable('WritableStreamDefaultController')
    );
    const writer = ws.getWriter();
    throws(
      () => structuredClone(writer),
      notSerializable('WritableStreamDefaultWriter')
    );
    writer.releaseLock();
  },
};
