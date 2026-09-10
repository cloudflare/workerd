// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Cross-implementation stream transfer over JS RPC: the runner worker has the
// typescript_implemented_streams compat flag, the peer worker does not, so every transfer
// below crosses implementations. The wire protocol is implementation-agnostic; each side
// constructs received streams with its own implementation, so received streams are always
// instanceof the receiver's own globals. All four serializer/deserializer pairings are
// covered: TypeScript-serialized -> legacy-deserialized (sendReadable/sendWritable) and
// legacy-serialized -> TypeScript-deserialized (receiveReadable/receivePair).

import { WorkerEntrypoint } from 'cloudflare:workers';
import * as assert from 'node:assert';

const enc = new TextEncoder();

// The legacy-flagged peer. (This same class is also exported by the runner service, but only
// the PEER binding's legacy-flagged instance is ever called.)
export class Peer extends WorkerEntrypoint {
  // Receives a runner-created (TypeScript-backed at origin) readable; it deserializes here
  // as this worker's own (legacy) implementation.
  async readFrom(stream) {
    assert.ok(stream instanceof ReadableStream);
    return await new Response(stream).text();
  }

  // Receives a runner-created writable and writes a fixed payload into it.
  async writeTo(stream) {
    assert.ok(stream instanceof WritableStream);
    const writer = stream.getWriter();
    await writer.write(enc.encode('written by the peer'));
    await writer.close();
  }

  // Returns a peer-created (legacy at origin) readable.
  makeReadable() {
    return new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('made by the peer'));
        c.close();
      },
    });
  }

  // Returns both halves of a peer-local identity transform, nested in an object (multiple
  // stream externals in one value graph). The runner writes into `writable` and reads the
  // echo from `readable`, so bytes traverse the wire in both directions through the peer's
  // transform.
  makeEchoPair() {
    const { readable, writable } = new IdentityTransformStream();
    return { readable, writable };
  }
}

export default {
  async test(controller, env) {
    // Both services embed this file, so the peer runs this test export too; only the runner
    // (which has the binding) performs the assertions.
    if (env.PEER === undefined) return;

    // TS-serialized -> legacy-deserialized (argument direction), readable.
    {
      const stream = new ReadableStream({
        start(c) {
          c.enqueue(enc.encode('made by the runner'));
          c.close();
        },
      });
      assert.strictEqual(await env.PEER.readFrom(stream), 'made by the runner');
    }

    // TS-serialized -> legacy-deserialized, writable (peer writes, runner reads back
    // through its own TypeScript-backed identity transform).
    {
      const { readable, writable } = new IdentityTransformStream();
      const promise = env.PEER.writeTo(writable);
      assert.strictEqual(
        await new Response(readable).text(),
        'written by the peer'
      );
      await promise;
    }

    // Legacy-serialized -> TS-deserialized (return direction), readable. The received
    // stream must be an instance of THIS worker's (TypeScript-implemented) global.
    {
      const stream = await env.PEER.makeReadable();
      assert.ok(stream instanceof ReadableStream);
      assert.strictEqual(await new Response(stream).text(), 'made by the peer');
    }

    // Legacy-serialized -> TS-deserialized, both directions at once through the peer's
    // echo pair, nested in an object.
    {
      const { readable, writable } = await env.PEER.makeEchoPair();
      assert.ok(readable instanceof ReadableStream);
      assert.ok(writable instanceof WritableStream);
      const writer = writable.getWriter();
      await writer.write(enc.encode('echoed through the peer'));
      await writer.close();
      assert.strictEqual(
        await new Response(readable).text(),
        'echoed through the peer'
      );
    }
  },
};
