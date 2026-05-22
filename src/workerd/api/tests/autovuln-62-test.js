// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-62.
// ValueReadable::read() sets reading=true before consumer->read() and
// reading=false after. ValueReadable::cancel() checks reading to decide
// whether to defer destruction. A re-entrant reader.read() inside the
// cancel callback (triggered from pull) clobbers reading=false, causing
// cancel() to immediately destroy the consumer while the outer
// consumer->read() → maybeDrainAndSetState() is still on the stack.
export const reentrantReadInCancelClobbersReadingFlag = {
  async test() {
    let reader;
    let cancelCalled = false;

    const rs = new ReadableStream(
      {
        pull(_controller) {
          // Inside: read() → ValueReadable::read() [reading=true]
          //   → ConsumerImpl::read() → handleRead → onConsumerWantsData → pull
          reader.cancel();
          // cancel() → ValueReadable::cancel() → controller->cancel()
          //   → user cancel() → inner reader.read()
          //   Inner read: reading=true → resolveAsDone (Closed) → reading=false
          //   Clobbers outer guard. cancel() sees reading=false → state=kj::none
          //   → Consumer FREED. On return, maybeDrainAndSetState() → UAF.
        },
        cancel(_reason) {
          cancelCalled = true;
          // Re-entrant inner read clobbers the `reading` flag.
          reader.read();
        },
      },
      { highWaterMark: 0 }
    );

    reader = rs.getReader();
    await reader.read();

    ok(cancelCalled, 'cancel callback should have been called');
  },
};
