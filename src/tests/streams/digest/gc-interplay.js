// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Interplay with garbage collection (the configs set --expose-gc).

import { strictEqual } from 'node:assert';

export const abandonedDigestNotSettledByGc = {
  async test() {
    // Collecting an unused stream must not settle its digest promise.
    let digestPromise;
    {
      digestPromise = new crypto.DigestStream('md5').digest;
    }
    gc();
    let state = 'pending';
    digestPromise.then(
      () => (state = 'resolved'),
      () => (state = 'rejected')
    );
    for (let i = 0; i < 10; i++) await Promise.resolve();
    await scheduler.wait(1);
    strictEqual(state, 'pending');
  },
};

export const writerKeepsCollectedStreamOperable = {
  async test() {
    const { writer, digestPromise } = (() => {
      const stream = new crypto.DigestStream('md5');
      return { writer: stream.getWriter(), digestPromise: stream.digest };
    })();
    gc();
    await writer.write('hello');
    await writer.close();
    strictEqual((await digestPromise).byteLength, 16);
  },
};
