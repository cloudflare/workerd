// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// FixedLengthStream length enforcement: writing more than expectedLength, or
// closing before delivering it, errors the stream. The error messages are
// shared ("too many bytes" / "did not see all expected bytes"), but the two
// implementations deliberately diverge on error type and surfacing point,
// and both sides are asserted per implementation:
// - C++ enforces on the READ side with TypeError: the offending write() or
//   close() itself succeeds, and the error appears at the reader.
// - TypeScript enforces EAGERLY on the write side with RangeError: the
//   offending write()/close() rejects, and the readable side errors too.

import { ok, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function expectLengthError(expectedType, messagePart) {
  return (err) => {
    ok(
      err instanceof expectedType,
      `Expected ${expectedType.name}, got ${err.constructor.name}: ${err.message}`
    );
    ok(err.message.includes(messagePart), `Unexpected message: ${err.message}`);
    return true;
  };
}

export const singleOverwriteErrorsReadable = {
  async test() {
    const fls = new FixedLengthStream(2);
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    if (usingTsImpl) {
      await rejects(
        writer.write(new Uint8Array([1, 2, 3])),
        expectLengthError(RangeError, 'too many bytes')
      );
      await rejects(
        reader.read(),
        expectLengthError(RangeError, 'too many bytes')
      );
    } else {
      // Read-side enforcement: the oversized write itself succeeds once the
      // read drains it; the read observes the error.
      const writePromise = writer.write(new Uint8Array([1, 2, 3]));
      await rejects(
        reader.read(),
        expectLengthError(TypeError, 'too many bytes')
      );
      await writePromise;
    }
  },
};

export const incrementalOverwriteErrorsReadable = {
  async test() {
    const fls = new FixedLengthStream(5);
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    // Write exactly 5 bytes; this is fine.
    const w1 = writer.write(new Uint8Array([1, 2, 3, 4, 5]));
    const r1 = await reader.read();
    await w1;
    ok(!r1.done);
    // The 6th byte exceeds the limit.
    if (usingTsImpl) {
      await rejects(
        writer.write(new Uint8Array([6])),
        expectLengthError(RangeError, 'too many bytes')
      );
      await rejects(
        reader.read(),
        expectLengthError(RangeError, 'too many bytes')
      );
    } else {
      const w2 = writer.write(new Uint8Array([6]));
      await rejects(
        reader.read(),
        expectLengthError(TypeError, 'too many bytes')
      );
      await w2;
    }
  },
};

export const underwriteErrorsStream = {
  async test() {
    const fls = new FixedLengthStream(10);
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    // Deliver 3 of 10 bytes, then close early.
    const w = writer.write(new Uint8Array([1, 2, 3]));
    await reader.read();
    await w;
    if (usingTsImpl) {
      await rejects(
        writer.close(),
        expectLengthError(RangeError, 'did not see all expected bytes')
      );
      await rejects(
        reader.read(),
        expectLengthError(RangeError, 'did not see all expected bytes')
      );
    } else {
      // Read-side enforcement: close() itself succeeds; the read observes
      // the error.
      await writer.close();
      await rejects(
        reader.read(),
        expectLengthError(TypeError, 'did not see all expected bytes')
      );
    }
  },
};

export const closeWithoutAnyWriteErrorsStream = {
  async test() {
    const fls = new FixedLengthStream(5);
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    if (usingTsImpl) {
      await rejects(
        writer.close(),
        expectLengthError(RangeError, 'did not see all expected bytes')
      );
      await rejects(
        reader.read(),
        expectLengthError(RangeError, 'did not see all expected bytes')
      );
    } else {
      await writer.close();
      await rejects(
        reader.read(),
        expectLengthError(TypeError, 'did not see all expected bytes')
      );
    }
  },
};

export const abortSkipsUnderwriteCheck = {
  async test() {
    // Abort is not close: aborting with undelivered bytes is not an
    // underwrite error.
    const fls = new FixedLengthStream(100);
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    const w = writer.write(new Uint8Array([1]));
    await reader.read();
    await w;
    // Must resolve without an underwrite error.
    await writer.abort(new Error('cancelled'));
  },
};
