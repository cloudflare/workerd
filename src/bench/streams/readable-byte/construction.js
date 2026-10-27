// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructing byte streams, through their start() and first pull(), which
// run as microtasks inside the timed call. Each call constructs 16 streams,
// since one costs about as much as the runner's overhead per call.

const COUNT = 16;

export const construction = {
  bench(b) {
    b.run(`type: 'bytes' x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) new ReadableStream({ type: 'bytes' });
    });
    b.run(`autoAllocateChunkSize x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        new ReadableStream({ type: 'bytes', autoAllocateChunkSize: 4096 });
      }
    });
    b.run(`then getReader({ mode: 'byob' }) x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        new ReadableStream({ type: 'bytes' }).getReader({ mode: 'byob' });
      }
    });
  },
};
