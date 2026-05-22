// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-60.
// ByteReadable::cancel() / ValueReadable::cancel() invoke the user's
// cancel callback synchronously. If the callback calls stream.tee(),
// tee() transitions the controller state to Closed (destroying the
// ByteReadable/ValueReadable whose cancel() is on the stack).
// cancel() then accesses freed memory (state = kj::none on freed this).
export const teeFromCancelCallbackFreesByteable = {
  async test() {
    let savedStream;
    let teeCalled = false;

    savedStream = new ReadableStream({
      type: 'bytes',
      cancel(_reason) {
        teeCalled = true;
        savedStream.tee();
      },
    });

    // cancel() on an unlocked stream — the cancel callback calls tee().
    await savedStream.cancel('foo');

    ok(teeCalled, 'cancel callback should have called tee()');
  },
};

// Same test with a value stream (ValueReadable path).
export const teeFromCancelCallbackFreesValueReadable = {
  async test() {
    let savedStream;
    let teeCalled = false;

    savedStream = new ReadableStream({
      cancel(_reason) {
        teeCalled = true;
        savedStream.tee();
      },
    });

    await savedStream.cancel('foo');

    ok(teeCalled, 'cancel callback should have called tee()');
  },
};
