// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-176.
// When a JS-backed ReadableStream closes during a pipe to an internal
// writable, deferControllerStateChange applies the pending Closed state
// and calls ReadableLockImpl::onClose(), which transitions readState
// from PipeLocked to Unlocked — destroying the PipeLocked that
// Pipe::State::source points to. During the subsequent async write,
// rs.locked is false so the attacker can call rs.getReader() to
// overwrite the OneOf storage with ReaderLocked. When the write
// resolves, pipeLoop performs a virtual call through the corrupted
// source reference → SIGSEGV.
export const pipeCloseDestroysPipeLockedDuringWrite = {
  async test() {
    const its = new IdentityTransformStream();
    const sinkReader = its.readable.getReader();

    const rs = new ReadableStream({
      start(c) {
        c.enqueue(new Uint8Array([1, 2, 3, 4]));
        c.close();
      },
    });

    const pipePromise = rs
      .pipeTo(its.writable, { preventClose: true })
      .catch((_e) => 'pipe-settled');

    // Let the pipe loop run: read → get data → start write → close applied.
    await scheduler.wait(0);
    await scheduler.wait(0);

    // After the close is applied by deferControllerStateChange → onClose(),
    // the PipeLocked should still be alive (fix), keeping rs.locked === true.
    // Pre-fix: rs.locked is false because onClose destroyed the PipeLocked.
    strictEqual(
      rs.locked,
      true,
      'rs should remain locked while pipe is active'
    );

    // Unblock the write by reading from the identity transform sink.
    await sinkReader.read();

    // Let the pipe loop settle.
    await scheduler.wait(0);

    // Now the pipe should have completed and released the lock.
    await pipePromise;
  },
};
