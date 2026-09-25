// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Pre-modern writer semantics, exercised with streams_enable_constructors
// as the ONLY flag: without writable_stream_spec_compliant_writer the
// writer-release paths keep their original behaviors, and without
// capture_async_api_throws some async methods throw synchronously.

import { strictEqual, ok, throws, rejects } from 'node:assert';

// A releaseLock() from inside strategy.size() does not doom the write:
// the write proceeds and fulfills (the spec-compliant flag makes it
// reject; see reentrancy.js in the main cells).
export const legacyReleaseLockInsideSizeWriteSucceeds = {
  async test() {
    let sinkWrote = false;
    let writer;
    const ws = new WritableStream(
      {
        write() {
          sinkWrote = true;
        },
      },
      {
        size() {
          writer.releaseLock();
          return 1;
        },
        highWaterMark: 10,
      }
    );
    writer = ws.getWriter();
    strictEqual(await writer.write('x'), undefined);
    strictEqual(sinkWrote, true);
    strictEqual(ws.locked, false);
  },
};

// releaseLock() leaves an already-resolved ready promise in place (the
// spec-compliant flag replaces it with a rejected one); the closed
// promise still rejects with the release TypeError.
export const legacyReleaseLockLeavesReadyFulfilled = {
  async test() {
    const ws = new WritableStream();
    const writer = ws.getWriter();
    await writer.ready;
    writer.releaseLock();
    strictEqual(await writer.ready, undefined);
    await rejects(writer.closed, {
      name: 'TypeError',
      message: 'This WritableStream writer has been released.',
    });
  },
};

// Without capture_async_api_throws a second close() throws synchronously
// (and with its own distinct message) instead of returning a rejected
// promise.
export const legacyDoubleCloseThrowsSync = {
  test() {
    const ws = new WritableStream({
      write() {},
    });
    const writer = ws.getWriter();
    writer.write(123);
    writer.close();
    throws(() => writer.close(), {
      name: 'TypeError',
      message: 'Cannot close a writer that is already being closed',
    });
  },
};

// Writing on a released writer returns a rejected promise even in the
// legacy configuration.
export const legacyWriteOnReleasedRejects = {
  async test() {
    const ws = new WritableStream({
      write() {},
    });
    const writer = ws.getWriter();
    writer.releaseLock();
    await rejects(writer.write('x'), {
      name: 'TypeError',
      message: 'This WritableStream writer has been released.',
    });
    ok(!ws.locked);
  },
};
