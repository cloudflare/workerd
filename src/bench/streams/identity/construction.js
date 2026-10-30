// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructing identity streams. Each call constructs 16 streams, since one
// costs about as much as the runner's overhead per call.

const COUNT = 16;

export const construction = {
  bench(b) {
    b.run(`IdentityTransformStream x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) new IdentityTransformStream();
    });
    b.run(`FixedLengthStream x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) new FixedLengthStream(65536);
    });
    b.run(
      `IdentityTransformStream, then getWriter() and getReader() x${COUNT}`,
      async () => {
        for (let i = 0; i < COUNT; i++) {
          const { readable, writable } = new IdentityTransformStream();
          writable.getWriter();
          readable.getReader();
        }
      }
    );
  },
};
