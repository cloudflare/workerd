// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A transform stream's whole life per call: a writer writes every chunk and
// closes while a reader reads the output to the end. The writer either
// waits for `ready` before each write, as a producer honoring backpressure
// does, or queues every write at once, as code that writes to a
// TransformStream from a callback often does.

import { CHUNK, TRANSFORMERS, drainReader, expectEqual } from 'helpers';

async function pump(name, count, { honorBackpressure }) {
  const { make, output } = TRANSFORMERS[name];
  const { readable, writable } = new TransformStream(make());
  const writer = writable.getWriter();
  const drained = drainReader(readable.getReader());
  const writes = [];
  for (let i = 0; i < count; i++) {
    if (honorBackpressure) await writer.ready;
    writes.push(writer.write(CHUNK));
  }
  const [read] = await Promise.all([drained, ...writes, writer.close()]);
  expectEqual(read, output(count), 'chunks read');
}

// Under --quick, which only checks that each case runs, every case writes
// a sixteenth as many chunks: a write and its read take tens of
// microtask turns, which are slow in sanitizer and debug builds.
const QUICK_DIVISOR = 16;

export const throughput = {
  bench(b) {
    const scale = (count) => (b.options.quick ? count / QUICK_DIVISOR : count);
    for (const count of [16, 256].map(scale)) {
      b.run(`await ready, no transformer x${count}`, () =>
        pump('no transformer', count, { honorBackpressure: true })
      );
    }
    const many = scale(256);
    for (const name of Object.keys(TRANSFORMERS)) {
      if (name === 'no transformer') continue;
      b.run(`await ready, ${name} x${many}`, () =>
        pump(name, many, { honorBackpressure: true })
      );
    }
    for (const name of ['no transformer', 'transform() enqueues']) {
      b.run(`queue every write(), ${name} x${many}`, () =>
        pump(name, many, { honorBackpressure: false })
      );
    }
  },
};
