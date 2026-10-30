// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A DigestStream's whole life per call: queue every write, close, and await
// the digest. Both implementations drive the same native digest context, so
// the small chunks isolate the cost of the stream around it.

import { SHAPES, chunkOf, expectDigest, shapeLabel } from 'helpers';

// Each algorithm with its digest's size in bytes.
const ALGORITHMS = {
  'SHA-256': 32,
  'SHA-1': 20,
  'SHA-512': 64,
  MD5: 16,
  crc32: 4,
};

async function digest(algorithm, count, size) {
  const stream = new crypto.DigestStream(algorithm);
  const writer = stream.getWriter();
  const chunk = chunkOf(size);
  const writes = [];
  for (let i = 0; i < count; i++) writes.push(writer.write(chunk));
  await Promise.all([...writes, writer.close()]);
  await expectDigest(stream, count * size, ALGORITHMS[algorithm]);
}

export const write = {
  bench(b) {
    for (const [count, size] of SHAPES) {
      b.run(`SHA-256 ${shapeLabel(count, size)}`, () =>
        digest('SHA-256', count, size)
      );
    }
    const [count, size] = SHAPES[1];
    for (const algorithm of Object.keys(ALGORITHMS)) {
      if (algorithm === 'SHA-256') continue;
      b.run(`${algorithm} ${shapeLabel(count, size)}`, () =>
        digest(algorithm, count, size)
      );
    }
    b.run(`string chunks, SHA-256 ${shapeLabel(count, size)}`, async () => {
      const stream = new crypto.DigestStream('SHA-256');
      const writer = stream.getWriter();
      const chunk = 'a'.repeat(size);
      const writes = [];
      for (let i = 0; i < count; i++) writes.push(writer.write(chunk));
      await Promise.all([...writes, writer.close()]);
      await expectDigest(stream, count * size, 32);
    });
  },
};
