// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// tee() of a value stream: both branches read concurrently, one read while
// the other is cancelled (what a discarded clone does), and both cancelled.

import { drainReader, enqueuedStream, expectEqual, pullStream } from 'helpers';

async function drainBoth([left, right], count) {
  const [a, b] = await Promise.all([
    drainReader(left.getReader()),
    drainReader(right.getReader()),
  ]);
  expectEqual(a, count, 'chunks read from the first branch');
  expectEqual(b, count, 'chunks read from the second branch');
}

export const tee = {
  bench(b) {
    for (const count of [16, 256]) {
      b.run(`read both branches, enqueued x${count}`, () =>
        drainBoth(enqueuedStream(count).tee(), count)
      );
    }
    b.run('read both branches, pull() x256', () =>
      drainBoth(pullStream(256).tee(), 256)
    );
    // The cancel is awaited alongside the read: under the TypeScript
    // implementation, as in the spec, a branch's cancel() settles only once
    // the source finishes or the other branch cancels too.
    b.run('read one branch, cancel the other x256', async () => {
      const [left, right] = enqueuedStream(256).tee();
      const [, count] = await Promise.all([
        right.cancel(),
        drainReader(left.getReader()),
      ]);
      expectEqual(count, 256, 'chunks read');
    });
    b.run('cancel both branches', async () => {
      const [left, right] = enqueuedStream(16).tee();
      await Promise.all([left.cancel(), right.cancel()]);
    });
  },
};
