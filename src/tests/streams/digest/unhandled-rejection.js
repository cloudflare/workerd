// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Rejection reporting for an abandoned digest promise. DELIBERATE
// divergence: the TypeScript implementation marks the digest promise
// handled, so disposing a stream whose digest nobody observed produces no
// unhandled-rejection report; the C++ implementation reports it. Marking
// handled must not leak to derived promises in either implementation.

import { strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

async function settleReports() {
  for (let i = 0; i < 10; i++) await Promise.resolve();
  await scheduler.wait(1);
}

export const abandonedDigestReporting = {
  async test() {
    const reports = [];
    const onReport = (event) => {
      reports.push(event.reason);
      event.preventDefault();
    };
    addEventListener('unhandledrejection', onReport);
    try {
      {
        const stream = new crypto.DigestStream('md5');
        stream[Symbol.dispose]();
      }
      await settleReports();
      strictEqual(
        reports.length,
        usingTsImpl ? 0 : 1,
        `abandoned digest reports: ${reports.map(String)}`
      );

      // A consumer that attaches a then() with no catch still gets a
      // report in both implementations.
      const before = reports.length;
      {
        const stream = new crypto.DigestStream('md5');
        stream.digest.then(() => {});
        stream[Symbol.dispose]();
      }
      await settleReports();
      strictEqual(reports.length, before + 1, 'derived promise should report');
      strictEqual(
        reports[reports.length - 1].message,
        'The DigestStream was disposed.'
      );
    } finally {
      removeEventListener('unhandledrejection', onReport);
    }
  },
};
