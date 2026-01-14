// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Test to validate timing semantics for JSRPC invocations with streaming responses.
// This test verifies that the Return event is emitted at the correct time relative
// to Onset and Outcome events.
//
// Expected timeline:
//   T=0:      Onset (invocation starts)
//   T=~500ms: Return (handler returns the stream)
//   T=~950ms: Outcome (stream fully consumed)

import { WorkerEntrypoint } from 'cloudflare:workers';

export class StreamingService extends WorkerEntrypoint {
  async getStreamWithDelays() {
    // Sleep 500ms before returning the stream
    await scheduler.wait(500);

    // Return a ReadableStream that takes ~450ms to drain
    const encoder = new TextEncoder();
    let chunksSent = 0;

    const stream = new ReadableStream({
      async pull(controller) {
        if (chunksSent >= 3) {
          controller.close();
          return;
        }
        // Sleep 150ms between chunks (3 chunks = ~450ms to drain)
        await scheduler.wait(150);
        controller.enqueue(encoder.encode(`chunk${chunksSent++}\n`));
      },
    });

    return stream;
  }
}

export default {
  async test(controller, env, ctx) {
    // Call the streaming RPC method
    const stream = await env.StreamingService.getStreamWithDelays();

    // Consume the stream fully
    const reader = stream.getReader();
    let chunks = [];
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(new TextDecoder().decode(value));
    }

    // Verify we got all chunks
    if (chunks.length !== 3) {
      throw new Error(`Expected 3 chunks, got ${chunks.length}`);
    }

    // The actual timing validation is done in the tail worker's test() handler
  },
};
