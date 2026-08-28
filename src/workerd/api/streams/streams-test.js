// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export const partiallyReadStream = {
  async test(ctrl, env, ctx) {
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      type: 'bytes',
      start(controller) {
        controller.enqueue(enc.encode('hello'));
        controller.enqueue(enc.encode('world'));
        controller.close();
      },
    });
    const reader = rs.getReader({ mode: 'byob' });
    await reader.read(new Uint8Array(5));
    reader.releaseLock();

    // Should not throw!
    await env.KV.put('key', rs);
  },
};

// Test for re-entrancy bug: when pushing to multiple consumers (via tee),
// the transform function can directly cancel another consumer synchronously.
// This should not crash - the cancelled consumer should gracefully ignore the push.
// Before the fix, this would crash with:
// "expected state.template tryGet<Ready>() != nullptr; The consumer is either closed or errored."
//
// ============================================================================
