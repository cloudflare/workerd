// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Cross-implementation stream transfer over JS RPC. Companion configs run this file with
// opposite implementation assignments, so streams originating from both implementations cross
// to the other implementation and back. The wire protocol is implementation-agnostic; each side
// constructs received streams with its own implementation, so received streams are always
// instanceof the receiver's own globals.

import { WorkerEntrypoint } from 'cloudflare:workers';
import * as assert from 'node:assert';

const enc = new TextEncoder();
const usingTsImpl =
  globalThis.Cloudflare.compatibilityFlags['typescript_implemented_streams'] ===
  true;

function makeConstructorBackedEchoPair() {
  let controller;
  const readable = new ReadableStream({
    start(c) {
      controller = c;
    },
  });
  const writable = new WritableStream({
    write(chunk) {
      controller.enqueue(chunk);
    },
    close() {
      controller.close();
    },
    abort(reason) {
      controller.error(reason);
    },
  });
  return { readable, writable };
}

export class Peer extends WorkerEntrypoint {
  usesTypeScriptStreams() {
    return usingTsImpl;
  }

  // Receives a caller-created readable; it deserializes as this worker's implementation.
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

  roundTrip(value) {
    return value;
  }

  // Returns a peer-created readable.
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

  // Unlike IdentityTransformStream, both halves exercise the constructor-backed stream paths.
  makeConstructorBackedEchoPair() {
    return makeConstructorBackedEchoPair();
  }
}

export default {
  async test(controller, env) {
    // Both services embed this file; only the runner in each config has the binding.
    if (env.PEER === undefined) return;

    // Keep this test honest if either config's flags are changed.
    assert.notStrictEqual(usingTsImpl, await env.PEER.usesTypeScriptStreams());

    // Caller-serialized -> peer-deserialized (argument direction), readable.
    {
      const stream = new ReadableStream({
        start(c) {
          c.enqueue(enc.encode('made by the runner'));
          c.close();
        },
      });
      assert.strictEqual(await env.PEER.readFrom(stream), 'made by the runner');
    }

    // Caller-serialized -> peer-deserialized, native/system-backed writable.
    {
      const { readable, writable } = new IdentityTransformStream();
      const promise = env.PEER.writeTo(writable);
      assert.strictEqual(
        await new Response(readable).text(),
        'written by the peer'
      );
      await promise;
    }

    // Caller-serialized -> peer-deserialized, constructor-backed writable. In the legacy
    // implementation this selects the writer-driven serializer rather than the native-sink path.
    {
      const { readable, writable } = makeConstructorBackedEchoPair();
      const promise = env.PEER.writeTo(writable);
      assert.strictEqual(
        await new Response(readable).text(),
        'written by the peer'
      );
      await promise;
    }

    // Peer-serialized -> caller-deserialized (return direction), readable. The received
    // stream must be an instance of this worker's global.
    {
      const stream = await env.PEER.makeReadable();
      assert.ok(stream instanceof ReadableStream);
      assert.strictEqual(await new Response(stream).text(), 'made by the peer');
    }

    // Peer-serialized -> caller-deserialized, both directions at once through the peer's
    // native/system-backed echo pair, nested in an object.
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

    // Peer-serialized -> caller-deserialized, constructor-backed readable and writable.
    {
      const { readable, writable } =
        await env.PEER.makeConstructorBackedEchoPair();
      assert.ok(readable instanceof ReadableStream);
      assert.ok(writable instanceof WritableStream);
      const writer = writable.getWriter();
      await writer.write(enc.encode('constructor-backed echo'));
      await writer.close();
      assert.strictEqual(
        await new Response(readable).text(),
        'constructor-backed echo'
      );
    }

    // Retransfer both stream values: caller -> peer -> caller. Running this under both configs
    // covers both mixed adapter compositions for RPC-backed streams.
    {
      const pair = await env.PEER.roundTrip(makeConstructorBackedEchoPair());
      assert.ok(pair.readable instanceof ReadableStream);
      assert.ok(pair.writable instanceof WritableStream);
      const text = new Response(pair.readable).text();
      const writer = pair.writable.getWriter();
      await writer.write(enc.encode('round-tripped across implementations'));
      await writer.close();
      assert.strictEqual(await text, 'round-tripped across implementations');
    }
  },
};
