// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// RPC stream transfer must not dispatch through user-patchable prototype methods:
// serialization of a TypeScript-backed stream drives the stream through internal
// (frozen cppExports) operations, so replacing WritableStream.prototype.getWriter or
// WritableStreamDefaultWriter.prototype.{write,close,abort} (or the readable-side
// getReader) must neither intercept the transferred data nor fake the transfer.

import { WorkerEntrypoint } from 'cloudflare:workers';
import * as assert from 'node:assert';

const enc = new TextEncoder();

export class Peer extends WorkerEntrypoint {
  // Writes a payload into a received writable, then closes it.
  async writeTo(stream) {
    const writer = stream.getWriter();
    await writer.write(enc.encode('delivered intact'));
    await writer.close();
  }

  // Reads a received readable fully.
  async readFrom(stream) {
    return await new Response(stream).text();
  }
}

export default {
  async test(controller, env) {
    // Both services embed this file; only the runner (which has the binding)
    // performs the assertions.
    if (env.PEER === undefined) return;

    const trapped = [];
    const trap = (name, impl) =>
      function (...args) {
        trapped.push(name);
        return impl.apply(this, args);
      };

    // Booby-trap the public stream surfaces BEFORE any transfer.
    const wsProto = WritableStream.prototype;
    const writerProto = WritableStreamDefaultWriter.prototype;
    const rsProto = ReadableStream.prototype;
    const origGetWriter = wsProto.getWriter;
    const origWrite = writerProto.write;
    const origClose = writerProto.close;
    const origAbort = writerProto.abort;
    const origGetReader = rsProto.getReader;
    wsProto.getWriter = trap('getWriter', origGetWriter);
    writerProto.write = trap('write', origWrite);
    writerProto.close = trap('close', origClose);
    writerProto.abort = trap('abort', origAbort);
    rsProto.getReader = trap('getReader', origGetReader);

    try {
      // Transfer a writable: the peer writes into it; the payload must arrive
      // through the real stream, with no patched method ever invoked.
      {
        const { readable, writable } = new IdentityTransformStream();
        const promise = env.PEER.writeTo(writable);
        const text = await new Response(readable).text();
        assert.strictEqual(text, 'delivered intact');
        await promise;
      }

      // Transfer a readable (serialization pumps it): same requirement.
      {
        const stream = new ReadableStream({
          start(c) {
            c.enqueue(enc.encode('pumped intact'));
            c.close();
          },
        });
        assert.strictEqual(await env.PEER.readFrom(stream), 'pumped intact');
      }

      assert.deepStrictEqual(
        trapped,
        [],
        `internal machinery dispatched through patched prototypes: ${trapped.join(', ')}`
      );
    } finally {
      wsProto.getWriter = origGetWriter;
      writerProto.write = origWrite;
      writerProto.close = origClose;
      writerProto.abort = origAbort;
      rsProto.getReader = origGetReader;
    }
  },
};
