// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Writer accounting under the synchronous hash sink: desiredSize counts
// the in-flight chunk against the default HWM of 1 and recovers once the
// write settles, in BOTH implementations (contrast the compression
// suite, where the C++ writer's desiredSize is inert). ready stays
// settled — the eager sink never sustains backpressure.

import { strictEqual } from 'node:assert';

export const desiredSizeCountsAndRecovers = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    strictEqual(writer.desiredSize, 1);
    const writePromise = writer.write(new Uint8Array(100_000));
    strictEqual(writer.desiredSize, 0);
    await writePromise;
    strictEqual(writer.desiredSize, 1);
    await writer.ready;
    await writer.close();
  },
};
