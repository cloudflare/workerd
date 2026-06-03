// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for heap-use-after-free in ReadableStream byte queue.
// Calling reader.cancel() inside the pull() callback destroys the consumer
// while ConsumerImpl::read() is still on the call stack.  The fix guards the
// maybeDrainAndSetState() call with a weak-ref check.
import { strictEqual } from 'node:assert';

export const cancelInsidePull = {
  async test() {
    let pullCalled = false;
    let reader;
    const stream = new ReadableStream({
      type: 'bytes',
      autoAllocateChunkSize: 1024,
      pull(controller) {
        pullCalled = true;
        reader.cancel('canceled from pull');
        return new Promise(() => {});
      },
    });
    reader = stream.getReader();
    const result = await reader.read();
    // After cancel, the read resolves as done.
    strictEqual(result.done, true);
    strictEqual(pullCalled, true);
    // Force GC to shake out any dangling pointers from the freed consumer.
    gc();
  },
};
