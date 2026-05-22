// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, strictEqual } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-37.
// When a BYOB request is invalidated (branch canceled) but sibling consumers
// still exist, respond(bytesWritten) should forward only the first
// bytesWritten bytes to the surviving branch — not the entire view.
export const teeInvalidatedRespondForwardsOnlyBytesWritten = {
  async test() {
    let ctrl;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        ctrl = c;
      },
    });

    const [a, b] = rs.tee();
    const readerA = a.getReader({ mode: 'byob' });
    const readerB = b.getReader({ mode: 'byob' });

    // Issue BYOB reads on both branches.
    const pa = readerA.read(new Uint8Array(8));
    const pb = readerB.read(new Uint8Array(8));

    // Capture the byobRequest before canceling branch A.
    const byobReq = ctrl.byobRequest;

    // Write 1 byte into the BYOB view.
    const view = byobReq.view;
    view[0] = 65; // 'A'

    // Cancel branch A — this invalidates the read request on that branch.
    await readerA.cancel('done with A');

    // Now respond with 1 byte. The invalidated-request branch should forward
    // only 1 byte to the surviving consumer (branch B), not the full 8-byte view.
    byobReq.respond(1);

    // Branch A was canceled — its read resolves as done.
    const r1 = await pa;
    strictEqual(r1.done, true, 'branch A should be done after cancel');

    // Branch B should receive exactly 1 byte: [65].
    const r2 = await pb;
    strictEqual(r2.done, false, 'branch B should not be done');
    strictEqual(r2.value.byteLength, 1, 'branch B should get exactly 1 byte');
    deepStrictEqual(
      [
        ...new Uint8Array(
          r2.value.buffer,
          r2.value.byteOffset,
          r2.value.byteLength
        ),
      ],
      [65],
      'branch B should get the byte that was written'
    );
  },
};
