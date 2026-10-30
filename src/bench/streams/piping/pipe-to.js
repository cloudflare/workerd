// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// pipeTo() from a value stream into a writable stream, to the end: sinks
// that settle each write synchronously or with a promise, a sink queue that
// lets the pipe run ahead, and a source that produces a chunk per pull().

import {
  countingWritable,
  enqueuedStream,
  expectEqual,
  pullStream,
} from 'helpers';

async function pipe(source, count, sinkOptions) {
  const sink = countingWritable(sinkOptions);
  await source.pipeTo(sink.stream);
  expectEqual(sink.writes, count, 'chunks written');
}

export const pipeTo = {
  bench(b) {
    for (const count of [16, 256]) {
      b.run(`sync sink x${count}`, () => pipe(enqueuedStream(count), count));
    }
    b.run('async sink x256', () =>
      pipe(enqueuedStream(256), 256, { async: true })
    );
    b.run('async sink, highWaterMark 16 x256', () =>
      pipe(enqueuedStream(256), 256, { async: true, highWaterMark: 16 })
    );
    b.run('pull() source x256', () => pipe(pullStream(256), 256));
  },
};
