// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The non-standard readAtLeast(minBytes, view) extension on BYOB readers:
// the read parks until at least minBytes have arrived, accumulating across
// multiple writes. The happy path is shared; argument validation diverges
// (ledger #18):
// - C++ validates minBytes as a C++ int against the buffer size, reporting
//   TypeError — with a negative value visibly sign-extended to
//   18446744073709551615 in the message before being rejected on the
//   element count (the seam byob-reader-resize-pending-read-test.js pins
//   at the jsg boundary).
// - TypeScript reports TypeError for a negative minimum and RangeError for
//   any minimum exceeding the view's capacity.

import { ok, strictEqual, rejects, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const readAtLeastWaitsForMinimum = {
  async test() {
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const reader = its.readable.getReader({ mode: 'byob' });
    const enc = new TextEncoder();

    let settled = false;
    const readPromise = reader
      .readAtLeast(4, new Uint8Array(20))
      .then((r) => ((settled = true), r));

    // Three bytes are not enough: the read stays parked, but the write is
    // fully consumed into it and settles.
    await writer.write(enc.encode('foo'));
    await scheduler.wait(0);
    strictEqual(settled, false);

    // Three more satisfy the minimum; everything accumulated is delivered.
    await writer.write(enc.encode('bar'));
    const { value, done } = await readPromise;
    strictEqual(done, false);
    strictEqual(new TextDecoder().decode(value), 'foobar');

    // At EOF, readAtLeast resolves done with a zero-length view like a
    // plain BYOB read.
    await writer.close();
    const tail = await reader.readAtLeast(4, new Uint8Array(10));
    strictEqual(tail.done, true);
    ok(tail.value instanceof Uint8Array);
    strictEqual(tail.value.byteLength, 0);
  },
};

export const readAtLeastValidation = {
  async test() {
    const its = new IdentityTransformStream();
    const reader = its.readable.getReader({ mode: 'byob' });
    if (usingTsImpl) {
      await rejects(
        async () => reader.readAtLeast(-1, new Uint8Array(64)),
        (err) =>
          err.constructor === TypeError &&
          /non-negative integer/.test(err.message)
      );
      for (const min of [2 ** 31, 2 ** 31 - 1, 21]) {
        await rejects(
          async () =>
            reader.readAtLeast(
              min,
              min === 21 ? new Uint8Array(20) : new Float64Array(8)
            ),
          (err) =>
            err.constructor === RangeError &&
            /must not exceed/.test(err.message)
        );
      }
    } else {
      // The negative value is sign-extended before the element-count check
      // rejects it; pinning the extended value in the message keeps the
      // seam visible.
      await rejects(
        async () => reader.readAtLeast(-1, new Uint8Array(64)),
        (err) =>
          err.constructor === TypeError &&
          /18446744073709551615/.test(err.message)
      );
      // Out of int range entirely: rejected at the jsg argument boundary.
      await rejects(
        async () => reader.readAtLeast(2 ** 31, new Float64Array(8)),
        (err) =>
          err.constructor === TypeError && /out of range/i.test(err.message)
      );
      // In range but exceeding the buffer: rejected on the element count.
      for (const [min, view] of [
        [2 ** 31 - 1, new Float64Array(8)],
        [21, new Uint8Array(20)],
      ]) {
        await rejects(
          async () => reader.readAtLeast(min, view),
          (err) =>
            err.constructor === TypeError &&
            /exceeds size of buffer/.test(err.message)
        );
      }
    }
  },
};

export const readAtLeastUnavailableOnDefaultReader = {
  test() {
    const its = new IdentityTransformStream();
    const reader = its.readable.getReader();
    strictEqual(typeof reader.readAtLeast, 'undefined');
    throws(() => reader.readAtLeast(1, new Uint8Array(4)), TypeError);
  },
};
