// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The strategy objects on their own: construction, size() and the
// highWaterMark getter. These queue no microtasks, so unlike the stream
// cases they are synchronous, and every result goes through b.blackBox().
// Each call performs 16 operations, since one costs about as much as the
// runner's overhead per call. Under C++, every access to `size` mints a new
// function (strategies test ledger #1), so its size() cases also measure
// creating one; the TypeScript implementation returns the same function.

import { CHUNK } from 'helpers';

const COUNT = 16;

export const strategyObjects = {
  bench(b) {
    b.run(`new CountQueuingStrategy() x${COUNT}`, () => {
      for (let i = 0; i < COUNT; i++) {
        b.blackBox(new CountQueuingStrategy({ highWaterMark: 16 }));
      }
    });
    b.run(`new ByteLengthQueuingStrategy() x${COUNT}`, () => {
      for (let i = 0; i < COUNT; i++) {
        b.blackBox(new ByteLengthQueuingStrategy({ highWaterMark: 16384 }));
      }
    });

    const count = new CountQueuingStrategy({ highWaterMark: 16 });
    const byteLength = new ByteLengthQueuingStrategy({ highWaterMark: 16384 });
    b.run(`CountQueuingStrategy size() x${COUNT}`, () => {
      for (let i = 0; i < COUNT; i++) b.blackBox(count.size(CHUNK));
    });
    b.run(`ByteLengthQueuingStrategy size() x${COUNT}`, () => {
      for (let i = 0; i < COUNT; i++) b.blackBox(byteLength.size(CHUNK));
    });
    b.run(`CountQueuingStrategy highWaterMark x${COUNT}`, () => {
      for (let i = 0; i < COUNT; i++) b.blackBox(count.highWaterMark);
    });
    b.run(`ByteLengthQueuingStrategy highWaterMark x${COUNT}`, () => {
      for (let i = 0; i < COUNT; i++) b.blackBox(byteLength.highWaterMark);
    });
  },
};
