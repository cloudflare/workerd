// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructing transform streams, through their start(), which settles as
// a microtask inside the timed call. Each call constructs 16 streams, since
// one costs about as much as the runner's overhead per call.

const COUNT = 16;

export const construction = {
  bench(b) {
    b.run(`no transformer x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) new TransformStream();
    });
    b.run(`start, transform and flush x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        new TransformStream({ start() {}, transform() {}, flush() {} });
      }
    });
    b.run(`both strategies x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        new TransformStream(
          {},
          new CountQueuingStrategy({ highWaterMark: 16 }),
          new CountQueuingStrategy({ highWaterMark: 16 })
        );
      }
    });
    b.run(`then getWriter() and getReader() x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) {
        const { readable, writable } = new TransformStream();
        writable.getWriter();
        readable.getReader();
      }
    });
  },
};
