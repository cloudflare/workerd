// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-91.
// Same root cause as AUTOVULN-60 but specifically for the ByteReadable
// path via reader.cancel(). The cancel callback calls releaseLock()
// then tee(), which destroys the ByteReadable via
// state.transitionTo<Closed> while cancel() is on the stack.
// Fixed by beginOperation/endOperation in cancel() (AUTOVULN-60) and
// deferTransitionTo in tee() (AUTOVULN-131).
export const byteReadableTeeFromReaderCancelCallback = {
  async test() {
    let stream;
    let reader;
    let teeCalled = false;

    stream = new ReadableStream({
      type: 'bytes',
      cancel(_reason) {
        teeCalled = true;
        reader.releaseLock();
        stream.tee();
      },
    });

    reader = stream.getReader();
    await reader.cancel('test-reason');

    ok(teeCalled, 'cancel callback should have called tee()');
  },
};

// Same test with ValueReadable (default stream, no type:'bytes').
export const valueReadableTeeFromReaderCancelCallback = {
  async test() {
    let stream;
    let reader;
    let teeCalled = false;

    stream = new ReadableStream({
      cancel(_reason) {
        teeCalled = true;
        reader.releaseLock();
        stream.tee();
      },
    });

    reader = stream.getReader();
    await reader.cancel('test-reason');

    ok(teeCalled, 'cancel callback should have called tee()');
  },
};
