// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Pre-flag abort semantics, as seen by workers predating
// internal_writable_stream_abort_clears_queue (2024-09-02): abort() WAITS
// for an in-flight write to be consumed instead of clearing it — so with no
// reader, both the write and the abort park until a read drains the write.
// (This wait-forever hazard is exactly what the flag later fixed.) Legacy
// suite: C++ implementation only; see identity-cpp-legacy.wd-test.

import { strictEqual, deepStrictEqual, rejects } from 'node:assert';

// Yields through the event loop so any settlement in flight lands before we
// assert on pending-ness.
const tick = () => scheduler.wait(0);

export const legacyAbortWaitsForPendingWrite = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    let writeSettled = false;
    let abortSettled = false;
    const writePromise = writer.write(new Uint8Array([1]));
    writePromise.then(
      () => (writeSettled = true),
      () => (writeSettled = true)
    );
    const abortPromise = writer.abort(new Error('abort while write pending'));
    abortPromise.then(
      () => (abortSettled = true),
      () => (abortSettled = true)
    );
    await tick();
    // Neither settles: the abort is waiting on the write, and the write is
    // waiting on a reader.
    strictEqual(writeSettled, false);
    strictEqual(abortSettled, false);
    // A read drains the write; only then does the abort complete.
    const reader = readable.getReader();
    const { value, done } = await reader.read();
    strictEqual(done, false);
    deepStrictEqual([...value], [1]);
    await writePromise;
    await abortPromise;
  },
};

export const legacyAbortWithPendingReadResolves = {
  async test() {
    // With no write in flight, legacy abort behaves like the modern one:
    // it resolves and rejects the pending read.
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const readPromise = reader.read();
    await writer.abort(new Error('boom'));
    await rejects(readPromise, Error);
  },
};
