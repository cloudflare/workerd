// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-156:
// Heap use-after-free read in readHelper().
//
// When a write is pending on an IdentityTransformStream and the
// write promise is canceled (via Canceler destruction in
// removeSink(), or via AbortSignal-triggered pipeTo cancellation),
// the WriteRequest's non-owning bytes pointer becomes dangling.
// readHelper() must detect that the write fulfiller is no longer
// waiting and transition to an error state instead of dereferencing
// the dangling pointer.
//
// This test uses a CompressionStream piped into an
// IdentityTransformStream with an AbortSignal to trigger the
// cancellation path. Post-fix, the read must reject with a
// disconnected error. Pre-fix, the read would succeed by reading
// from freed memory (a heap-use-after-free detectable under ASAN).

import { strictEqual, rejects } from 'node:assert';

export const regressionWriteCancelThenRead = {
  async test() {
    const its = new IdentityTransformStream();
    const reader = its.readable.getReader({ mode: 'byob' });

    const cs = new CompressionStream('gzip');
    const csWriter = cs.writable.getWriter();
    const ac = new AbortController();

    // Pipe compressed output into the identity transform stream.
    // preventAbort:true means the abort won't call sink->abort(),
    // leaving the WriteRequest with a canceled fulfiller.
    const pipePromise = cs.readable
      .pipeTo(its.writable, {
        signal: ac.signal,
        preventAbort: true,
      })
      .catch(() => {});

    // Write data to generate output that parks a WriteRequest in
    // the IdentityTransformStreamImpl.
    const SIZE = 65536;
    await csWriter.write(new Uint8Array(SIZE).fill(0x41));

    // Let the compressed data flow through.
    await scheduler.wait(10);

    // Abort the pipe. This cancels the pumpTo coroutine via
    // kj::Canceler, freeing the coroutine frame that backs the
    // WriteRequest.bytes pointer.
    ac.abort();
    await scheduler.wait(10);

    // Post-fix, readHelper() checks fulfiller->isWaiting() and
    // transitions to DISCONNECTED, causing the read to reject.
    // Pre-fix, readHelper() would memmove from freed memory.
    await rejects(
      reader.read(new Uint8Array(SIZE)),
      (err) => {
        strictEqual(err instanceof Error, true);
        return true;
      },
      'read() should reject after write cancellation (UAF guard)'
    );

    await pipePromise;
  },
};
