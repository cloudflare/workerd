// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructing encoding streams. Each call constructs 16 streams, since one
// costs about as much as the runner's overhead per call.

const COUNT = 16;

export const construction = {
  bench(b) {
    b.run(`TextEncoderStream x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) new TextEncoderStream();
    });
    b.run(`TextDecoderStream x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) new TextDecoderStream();
    });
    b.run(`TextDecoderStream, fatal x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        new TextDecoderStream('utf-8', { fatal: true });
      }
    });
  },
};
