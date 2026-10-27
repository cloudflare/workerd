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
  sizeLabel,
} from 'helpers';

export const read = {
  bench(b) {
    for (const size of [4096, 65536, 1048576]) {
      const bytes = bytesOf(size);
      b.run(`default reader, ${sizeLabel(size)}`, async () => {
        const reader = new Response(bytes).body.getReader();
        expectEqual(await drainBytes(reader), size, 'bytes read');
      });
    }
    for (const size of [65536, 1048576]) {
      const bytes = bytesOf(size);
      b.run(`BYOB reader into 16 KiB views, ${sizeLabel(size)}`, async () => {
        const reader = new Response(bytes).body.getReader({ mode: 'byob' });
        expectEqual(await drainByob(reader, 16384), size, 'bytes read');
      });
    }
    const size = 1048576;
    const bytes = bytesOf(size);
    b.run(`for await, ${sizeLabel(size)}`, async () => {
      let read = 0;
      for await (const chunk of new Response(bytes).body) {
        read += chunk.byteLength;
      }
      expectEqual(read, size, 'bytes read');
    });
    const text = 'a'.repeat(65536);
    b.run('default reader, 64 KiB string body', async () => {
      const reader = new Response(text).body.getReader();
      expectEqual(await drainBytes(reader), text.length, 'bytes read');
    });
    const blob = new Blob([bytesOf(65536)]);
    b.run('Blob.stream(), default reader, 64 KiB', async () => {
      const reader = blob.stream().getReader();
      expectEqual(await drainBytes(reader), blob.size, 'bytes read');
    });
  },
};
