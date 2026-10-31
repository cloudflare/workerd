// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructing writable streams, through their start(), which settles as a
// microtask inside the timed call. Each call constructs 16 streams, since
// one costs about as much as the runner's overhead per call.

const COUNT = 16;

export const construction = {
  bench(b) {
    b.run(`no sink x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) new WritableStream();
    });
    b.run(`start, write, close and abort x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        new WritableStream({ start() {}, write() {}, close() {}, abort() {} });
      }
    });
    b.run(`CountQueuingStrategy x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        new WritableStream({}, new CountQueuingStrategy({ highWaterMark: 16 }));
      }
    });
    b.run(`then getWriter() x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) new WritableStream().getWriter();
    });
  },
};
