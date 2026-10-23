// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// pipeTo() into a DigestStream: from a JavaScript value stream, and from a
// Response body, the shape of a worker hashing what it fetched.

import { SHAPES, chunkOf, expectDigest, shapeLabel, sizeLabel } from 'helpers';

function valueStream(count, size) {
  const chunk = chunkOf(size);
  return new ReadableStream({
    start(controller) {
      for (let i = 0; i < count; i++) controller.enqueue(chunk);
      controller.close();
    },
  });
}

export const pipe = {
  bench(b) {
    for (const [count, size] of SHAPES) {
      b.run(
        `from a value stream, SHA-256 ${shapeLabel(count, size)}`,
        async () => {
          const stream = new crypto.DigestStream('SHA-256');
          await valueStream(count, size).pipeTo(stream);
          await expectDigest(stream, count * size, 32);
        }
      );
    }
    for (const size of [4096, 65536, 1048576]) {
      const body = new Uint8Array(size);
      b.run(`from a Response body, SHA-256 ${sizeLabel(size)}`, async () => {
        const stream = new crypto.DigestStream('SHA-256');
        await new Response(body).body.pipeTo(stream);
        await expectDigest(stream, size, 32);
      });
    }
  },
};
