// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructing value streams, through their start() and first pull(), which
// run as microtasks inside the timed call. Each call constructs 16 streams,
// since one costs about as much as the runner's overhead per call.

const COUNT = 16;

export const construction = {
  bench(b) {
    b.run(`no source x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) new ReadableStream();
    });
    b.run(`start, pull and cancel x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        new ReadableStream({ start() {}, pull() {}, cancel() {} });
      }
    });
    b.run(`CountQueuingStrategy x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        new ReadableStream({}, new CountQueuingStrategy({ highWaterMark: 16 }));
      }
    });
    b.run(`size() strategy x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        new ReadableStream({}, { highWaterMark: 16, size: () => 1 });
      }
    });
    b.run(`then getReader() x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) new ReadableStream().getReader();
    });
  },
};
