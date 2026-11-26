// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Runtime-produced bodies read from JavaScript: a Response's body with
// default and BYOB readers and for await, and Blob.stream().

import {
  bytesOf,
  drainByob,
  drainBytes,
  expectEqual,
  scaled,
  sizeLabel,
} from 'helpers';

export const read = {
  bench(b) {
    const [small, medium, large] = [4096, 65536, 1048576].map((size) =>
      scaled(b, size)
    );
    for (const size of [small, medium, large]) {
      const bytes = bytesOf(size);
      b.run(`default reader, ${sizeLabel(size)}`, async () => {
        const reader = new Response(bytes).body.getReader();
        expectEqual(await drainBytes(reader), size, 'bytes read');
      });
    }
    const view = scaled(b, 16384);
    for (const size of [medium, large]) {
      const bytes = bytesOf(size);
      b.run(
        `BYOB reader into ${sizeLabel(view)} views, ${sizeLabel(size)}`,
        async () => {
          const reader = new Response(bytes).body.getReader({ mode: 'byob' });
          expectEqual(await drainByob(reader, view), size, 'bytes read');
        }
      );
    }
    const bytes = bytesOf(large);
    b.run(`for await, ${sizeLabel(large)}`, async () => {
      let read = 0;
      for await (const chunk of new Response(bytes).body) {
        read += chunk.byteLength;
      }
      expectEqual(read, large, 'bytes read');
    });
    const text = 'a'.repeat(medium);
    b.run(`default reader, ${sizeLabel(medium)} string body`, async () => {
      const reader = new Response(text).body.getReader();
      expectEqual(await drainBytes(reader), text.length, 'bytes read');
    });
    const blob = new Blob([bytesOf(medium)]);
    b.run(`Blob.stream(), default reader, ${sizeLabel(medium)}`, async () => {
      const reader = blob.stream().getReader();
      expectEqual(await drainBytes(reader), blob.size, 'bytes read');
    });
  },
};
