// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for JSRPC tracing: when an RPC method receives a callback/stub as an argument
// and invokes it, the follow-up jsRpcCall (the server calling back into the client) must nest
// under the server-side jsRpcCall span of the method that received the callback -- not become a
// sibling of it under the onset. The parent/child relationship is asserted in the tail worker's
// test() handler (see jsrpc-callback-trace-test-tail.js).

import { WorkerEntrypoint } from 'cloudflare:workers';

export class CallbackService extends WorkerEntrypoint {
  // Receives a callback (serialized as an RPC stub) and invokes it. Invoking `cb` makes an RPC
  // back to the caller, producing a client-side jsRpcCall span within this invocation's trace.
  async invokeCallback(cb) {
    return await cb();
  }
}

export default {
  async test(controller, env, ctx) {
    const result = await env.CallbackService.invokeCallback(() => 42);
    if (result !== 42) {
      throw new Error(`Expected callback result 42, got ${result}`);
    }
  },
};
