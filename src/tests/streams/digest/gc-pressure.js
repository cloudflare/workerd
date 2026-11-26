// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A major GC that finishes inside a sink call must not destroy the stream
// under it. Run alone by digest-gc-pressure-{cpp,ts}.wd-test, whose V8 flags
// make that GC likely.
//
// Once a writer has queued a close and released its lock, no JS holds the
// stream. The close sink allocates on the cppgc heap when it resolves the
// digest, and an allocation that crosses cppgc's periodic limit check while
// V8 is marking and over its allocation limit finishes the GC on the spot,
// sweeping the stream mid-call. --stress-incremental-marking keeps marking
// in progress almost all the time; the 1 MiB buffer each round drops puts
// the heap over its limit.

export const gcDuringCloseOfDroppedStream = {
  async test() {
    const pressure = [];
    for (let round = 0; round < 400; round++) {
      for (let i = 0; i < 25; i++) {
        const writer = new crypto.DigestStream('SHA-256').getWriter();
        writer.write(new Uint8Array(16));
        writer.close();
        writer.releaseLock();
      }
      // Drop last round's buffer before allocating this one, so it is
      // garbage, not live memory, while the closes run.
      pressure.length = 0;
      pressure.push(new ArrayBuffer(1 << 20));
      await scheduler.wait(0);
    }
  },
};
