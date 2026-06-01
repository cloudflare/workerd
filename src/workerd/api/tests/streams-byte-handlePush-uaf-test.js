// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-95:
// Heap use-after-free in ByteQueue::handlePush via re-entrant
// ReadableByteStreamController.error() during BYOB read resolution.
//
// The attack: a BYOB reader issues a read, then the controller enqueues data.
// handlePush resolves the pending read via request->resolve(js), which triggers
// V8's promise resolution thenable check (Get(resolution, "then")). A malicious
// Object.prototype.then getter calls controller.error(), which transitions the
// ConsumerImpl from Ready to Errored, freeing the Ready storage. After resolve()
// returns, handlePush's while loop checks state.readRequests.empty() — a
// use-after-free on the freed Ready storage.
//
// Under ASAN this crashes immediately. Without ASAN the test verifies behavioral
// correctness: the runtime does not assert/crash and the read resolves with data.
import { strictEqual } from 'node:assert';

export const handlePushReentrantError = {
  async test() {
    let ctrl;
    const stream = new ReadableStream({
      type: 'bytes',
      start(controller) {
        ctrl = controller;
      },
    });

    const reader = stream.getReader({ mode: 'byob' });

    // Issue a BYOB read that will be pending until data is enqueued.
    const readPromise = reader.read(new Uint8Array(16));

    // Install a malicious Object.prototype.then getter that calls
    // controller.error() during promise resolution, triggering re-entrant
    // state destruction while handlePush still holds a Ready& reference.
    let thenCalled = false;
    Object.defineProperty(Object.prototype, 'then', {
      get() {
        // Only trigger once to avoid infinite recursion.
        delete Object.prototype.then;
        thenCalled = true;
        try {
          ctrl.error(new Error('re-entrant error from then getter'));
        } catch {
          // controller.error() may throw if the controller state has
          // already changed. That's fine.
        }
        return undefined;
      },
      configurable: true,
    });

    try {
      // Enqueue data — this calls handlePush which resolves the pending
      // BYOB read, triggering the Object.prototype.then getter above.
      // Pre-fix, this would cause a heap use-after-free when the while
      // loop continued after resolve() returned.
      ctrl.enqueue(new Uint8Array([1, 2, 3, 4]));
    } catch {
      // The enqueue may throw because the stream was errored re-entrantly.
    }

    // Clean up the then getter in case it wasn't triggered.
    delete Object.prototype.then;

    // The read was resolved with data before the error was triggered
    // (handlePush resolves the read, then V8's thenable check fires).
    const result = await readPromise;
    strictEqual(result.done, false);
    strictEqual(result.value.byteLength, 4);
    strictEqual(result.value[0], 1);
    strictEqual(thenCalled, true);

    // Allocate objects to pressure the allocator into reclaiming freed memory,
    // making the UAF more likely to manifest under ASAN.
    for (let i = 0; i < 100; i++) {
      new ReadableStream({ type: 'bytes', start() {} });
    }

    // Force GC to shake out any dangling pointers from the freed consumer.
    gc();
  },
};
