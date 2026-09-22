// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Strategy instances handed to stream constructors: the stream reads
// highWaterMark and size and applies them to desiredSize accounting.

import { strictEqual, ok } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const strategiesDriveReadableDesiredSize = {
  test() {
    {
      const strategy = new ByteLengthQueuingStrategy({ highWaterMark: 10 });
      let startRan = false;
      new ReadableStream(
        {
          start(c) {
            strictEqual(c.desiredSize, 10);
            c.enqueue(new Uint8Array(2));
            strictEqual(c.desiredSize, 8);
            startRan = true;
          },
        },
        strategy
      );
      ok(startRan);
    }
    {
      const strategy = new CountQueuingStrategy({ highWaterMark: 9 });
      let startRan = false;
      new ReadableStream(
        {
          start(c) {
            strictEqual(c.desiredSize, 9);
            c.enqueue(new Uint8Array(2));
            strictEqual(c.desiredSize, 8);
            startRan = true;
          },
        },
        strategy
      );
      ok(startRan);
    }
  },
};

export const strategiesDriveWritableDesiredSize = {
  async test() {
    const writable = new WritableStream(
      { write() {} },
      new ByteLengthQueuingStrategy({ highWaterMark: 10 })
    );
    const writer = writable.getWriter();
    strictEqual(writer.desiredSize, 10);
    const writePromise = writer.write(new Uint8Array(4));
    strictEqual(writer.desiredSize, 6);
    await writePromise;
    strictEqual(writer.desiredSize, 10);
    await writer.close();
  },
};

export const classSizeUsableInPlainStrategyBag = {
  test() {
    // The class size function works detached, inside a plain strategy bag.
    const size = new ByteLengthQueuingStrategy({ highWaterMark: 1 }).size;
    let observed;
    new ReadableStream(
      {
        start(c) {
          c.enqueue(new Uint8Array(7));
          observed = c.desiredSize;
        },
      },
      { highWaterMark: 10, size }
    );
    strictEqual(observed, 3);
  },
};

export const fractionalAndZeroHighWaterMarks = {
  test() {
    {
      // Divergence: the strategy object reflects a fractional highWaterMark
      // faithfully, but the C++ stream truncates it to an integer in its
      // queue accounting; TypeScript keeps the spec's fractional value.
      let observed;
      new ReadableStream(
        {
          start(c) {
            observed = c.desiredSize;
          },
        },
        new CountQueuingStrategy({ highWaterMark: 0.5 })
      );
      strictEqual(observed, usingTsImpl ? 0.5 : 0);
    }
    {
      let observed;
      new ReadableStream(
        {
          start(c) {
            observed = c.desiredSize;
          },
        },
        new CountQueuingStrategy({ highWaterMark: 0 })
      );
      strictEqual(observed, 0);
    }
  },
};
