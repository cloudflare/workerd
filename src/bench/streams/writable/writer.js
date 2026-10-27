// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The write path on long-lived streams, so construction is out of the
// picture: each call performs 16 operations, since one costs about as much
// as the runner's overhead per call. The streams never close, and each call
// leaves its stream as it found it.

import { CHUNK, countingSink, expectEqual } from 'helpers';

const COUNT = 16;

function writeMany(sink, writer) {
  return async () => {
    const expected = sink.writes + COUNT;
    for (let i = 0; i < COUNT; i++) await writer.write(CHUNK);
    expectEqual(sink.writes, expected, 'chunks written');
  };
}

export const writer = {
  bench(b) {
    const idle = new WritableStream();
    b.run(`getWriter() + releaseLock() x${COUNT}`, async () => {
      for (let i = 0; i < COUNT; i++) idle.getWriter().releaseLock();
    });

    const sink = countingSink();
    b.run(
      `write(), sync sink x${COUNT}`,
      writeMany(sink, new WritableStream(sink).getWriter())
    );

    const asyncSink = countingSink({ async: true });
    b.run(
      `write(), async sink x${COUNT}`,
      writeMany(asyncSink, new WritableStream(asyncSink).getWriter())
    );

    const readySink = countingSink({ async: true });
    const readyWriter = new WritableStream(readySink).getWriter();
    b.run(`await ready, then write() x${COUNT}`, async () => {
      const expected = readySink.writes + COUNT;
      for (let i = 0; i < COUNT; i++) {
        await readyWriter.ready;
        await readyWriter.write(CHUNK);
      }
      expectEqual(readySink.writes, expected, 'chunks written');
    });
  },
};
