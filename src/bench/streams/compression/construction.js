// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructing compression streams, each of which sets up a zlib stream,
// and closing them unused. Each call handles 16 streams, since one costs
// about as much as the runner's overhead per call.

const COUNT = 16;

async function closeUnused(stream) {
  const reader = stream.readable.getReader();
  await stream.writable.close();
  while (!(await reader.read()).done);
}

export const construction = {
  bench(b) {
    b.run(`CompressionStream('gzip') x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) new CompressionStream('gzip');
    });
    b.run(`DecompressionStream('gzip') x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) new DecompressionStream('gzip');
    });
    b.run(`CompressionStream('gzip'), closed unused x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        await closeUnused(new CompressionStream('gzip'));
      }
    });
  },
};
