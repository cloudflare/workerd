// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, rejects } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-90.
// Same root cause as AUTOVULN-131 but for the ValueReadable path
// (default reader) instead of ByteReadable (BYOB reader).
// tee() from inside pull() during a read destroys the kj::Own<ValueReadable>
// via state.transitionTo<Closed>. Fixed by using deferTransitionTo in tee().
export const valueReadableTeeFromPullDuringRead = {
  async test() {
    let rs;
    let reader;
    let triggered = false;
    let teeResult;

    rs = new ReadableStream(
      {
        pull(_controller) {
          if (triggered) return;
          triggered = true;
          reader.releaseLock();
          teeResult = rs.tee();
        },
      },
      { highWaterMark: 0 }
    );

    // Let start() resolve so flags.started=true.
    for (let i = 0; i < 5; i++) await Promise.resolve();

    reader = rs.getReader();
    await rejects(reader.read(), {
      message: /This ReadableStream reader has been released/,
    });

    ok(triggered, 'pull should have been called');
    ok(teeResult, 'tee() should have returned');
  },
};
