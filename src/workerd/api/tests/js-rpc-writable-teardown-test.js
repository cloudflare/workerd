// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A WritableStream transferred over JS RPC and then abandoned -- the peer writes into it but
// never closes it, and the runner drops the stream -- leaves the RPC adapter holding a live
// writer when it is dropped. Both the adapter's destructor and its revoke path then reach into
// the IoContext to run the writer's abort algorithm, and that can happen after the last
// IncomingRequest is gone (during TaskSet teardown). IoContext::run() requires a current
// IncomingRequest, so the abort must be skipped in that window rather than attempted.
//
// Same-implementation and cross-implementation configurations embed this file. Without
// typescript_implemented_streams the JS-backed sink below has no WritableStreamSink to detach and
// serializes through the legacy adapter; with the flag it serializes through the TypeScript writer
// sink. The cross-implementation configurations also cover each received-stream implementation.

import { WorkerEntrypoint } from 'cloudflare:workers';
import * as assert from 'node:assert';

const enc = new TextEncoder();
const usingTsImpl =
  globalThis.Cloudflare.compatibilityFlags['typescript_implemented_streams'] ===
  true;

export class Peer extends WorkerEntrypoint {
  usesTypeScriptStreams() {
    return usingTsImpl;
  }

  // Writes into the received writable and returns without closing it, so the writer is still
  // live when the runner drops the stream.
  async writeAndAbandon(stream) {
    assert.ok(stream instanceof WritableStream);
    const writer = stream.getWriter();
    await writer.write(enc.encode('partial'));
  }
}

export default {
  async test(controller, env) {
    // Both services embed this file; only the runner has the binding.
    if (env.PEER === undefined) return;

    if (env.EXPECT_CROSS_IMPL !== undefined) {
      assert.notStrictEqual(
        usingTsImpl,
        await env.PEER.usesTypeScriptStreams()
      );
    }

    // A JS-backed sink accepts each chunk immediately, so the peer's write settles without
    // anything draining the stream and the call returns with the writer still held.
    const chunks = [];
    const writable = new WritableStream({
      write(chunk) {
        chunks.push(chunk);
      },
    });

    await env.PEER.writeAndAbandon(writable);
    assert.strictEqual(chunks.length, 1);

    // `writable` goes out of scope here, still unclosed and still holding the peer's writer.
    // Its adapter is dropped during request teardown.
  },
};
