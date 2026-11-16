// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructing DigestStreams, and closing them unwritten. Each call handles
// 16 streams, since one costs about as much as the runner's overhead per
// call.

import { expectDigest } from 'helpers';

const COUNT = 16;

export const construction = {
  bench(b) {
    b.run(`SHA-256 x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) new crypto.DigestStream('SHA-256');
    });
    b.run(`SHA-256, closed unwritten x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        const stream = new crypto.DigestStream('SHA-256');
        await stream.getWriter().close();
        await expectDigest(stream, 0, 32);
      }
    });
  },
};
