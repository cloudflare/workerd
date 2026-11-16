// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// structuredClone over ReadableStreams and the objects they hand out.
// Transferable streams (WHATWG §8.2) are UNIMPLEMENTED in both
// implementations: any { transfer: [stream] } rejects TypeError. Plain
// (non-transfer) cloning rejects DataCloneError in both, as for any
// platform object without a serialization; only the stream's message
// DIVERGES: C++ names the RPC-only serialization, TypeScript gives the
// generic message (its streams are serializable over RPC all the same).
// The async iterator's message names its class string, which diverges
// (ledger #13). The original stream is untouched either way.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function notSerializable(type) {
  return {
    name: 'DataCloneError',
    message: `Could not serialize object of type "${type}". This type does not support serialization.`,
  };
}

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
      throws(
        () => structuredClone(rs),
        usingTsImpl
          ? notSerializable('ReadableStream')
          : {
              name: 'DataCloneError',
              message: 'ReadableStream can only be serialized for RPC.',
            }
      );
      // Nested in a value, as well as at the top level.
      throws(() => structuredClone({ rs }), { name: 'DataCloneError' });
      strictEqual(rs.locked, false); // original untouched
    }
  },
};

export const structuredCloneReadableParts = {
  async test() {
    let defaultController;
    const rs = new ReadableStream({
      start(c) {
        defaultController = c;
      },
    });
    let byteController;
    const bytes = new ReadableStream({
      type: 'bytes',
      start(c) {
        byteController = c;
      },
    });
    const parts = [
      ['ReadableStreamDefaultController', defaultController],
      ['ReadableByteStreamController', byteController],
    ];
    for (const [type, part] of parts) {
      throws(() => structuredClone(part), notSerializable(type));
    }

    const reader = rs.getReader();
    throws(
      () => structuredClone(reader),
      notSerializable('ReadableStreamDefaultReader')
    );
    reader.releaseLock();

    const byobReader = bytes.getReader({ mode: 'byob' });
    throws(
      () => structuredClone(byobReader),
      notSerializable('ReadableStreamBYOBReader')
    );
    const read = byobReader.read(new Uint8Array(4));
    throws(
      () => structuredClone(byteController.byobRequest),
      notSerializable('ReadableStreamBYOBRequest')
    );
    byteController.enqueue(new Uint8Array([1]));
    strictEqual((await read).value.length, 1);
    byobReader.releaseLock();

    const iterator = rs.values();
    throws(
      () => structuredClone(iterator),
      notSerializable(
        usingTsImpl
          ? 'ReadableStream AsyncIterator'
          : 'ReadableStreamAsyncIterator'
      )
    );
    await iterator.return();
  },
};
