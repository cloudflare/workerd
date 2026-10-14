// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for WritableStreamInternalController::drain() settling queued events whose
// GC-visited handles are no longer kept alive.
//
// Once a GC has traced the destination's queue, the promise resolvers (and the queued Pipe's
// references) in it are weak handles that stay valid only while they keep being traced through
// the queue. drain() runs user JS when it cancels a JS-backed pipe source, so a GC can happen in
// the middle of the drain. If the events have been taken out of the queue by then without being
// moved individually, that GC collects them, and settling them afterwards crashes.
import { strictEqual, rejects } from 'node:assert';

export const drainSurvivesGcInSourceCancel = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();

    // A write that stays in flight: nothing reads from `readable`, so the identity transform
    // holds the write until a read arrives or the readable side is cancelled.
    const writer = writable.getWriter();
    const write = writer.write(new Uint8Array(16));
    writer.releaseLock();

    // Queue a pipe behind the stalled write. The source never produces a chunk, so the Pipe
    // sits in the destination's queue until the drain cancels it, and the cancel algorithm
    // forces a full GC while drain() is on the stack.
    let cancelled = false;
    const source = new ReadableStream({
      pull() {
        return new Promise(() => {});
      },
      cancel() {
        cancelled = true;
        gc();
      },
    });
    // The pipe promise is deliberately not retained: the only thing keeping its resolver
    // alive is the queued Pipe event, which is exactly what the drain must keep traced.
    source.pipeTo(writable);

    // Trace the queue once so that its handles are in the weak, traced state.
    gc();

    // Cancelling the readable side fails the in-flight write, and the write's rejection
    // drains the rest of the queue, cancelling the source from inside drain().
    await readable.cancel(new Error('boom'));
    await rejects(write);
    strictEqual(cancelled, true);
  },
};
