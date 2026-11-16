// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A Response's body piped onward: into a JavaScript sink, through a
// JavaScript transform into a new Response, straight into a new Response,
// and through TextDecoderStream.

import { bytesOf, drainBytes, expectEqual, sizeLabel } from 'helpers';

const SIZES = [65536, 1048576];

export const pipe = {
  bench(b) {
    for (const size of SIZES) {
      const bytes = bytesOf(size);
      b.run(`pipeTo() a WritableStream, ${sizeLabel(size)}`, async () => {
        let written = 0;
        await new Response(bytes).body.pipeTo(
          new WritableStream({
            write(chunk) {
              written += chunk.byteLength;
            },
          })
        );
        expectEqual(written, size, 'bytes written');
      });
    }
    for (const size of SIZES) {
      const bytes = bytesOf(size);
      b.run(
        `pipeThrough() a TransformStream into a Response, ${sizeLabel(size)}`,
        async () => {
          const body = new Response(bytes).body.pipeThrough(
            new TransformStream()
          );
          const read = await new Response(body).arrayBuffer();
          expectEqual(read.byteLength, size, 'bytes read');
        }
      );
    }
    for (const size of SIZES) {
      const bytes = bytesOf(size);
      b.run(`into a new Response, ${sizeLabel(size)}`, async () => {
        const read = await new Response(new Response(bytes).body).arrayBuffer();
        expectEqual(read.byteLength, size, 'bytes read');
      });
    }
    const size = 65536;
    const bytes = bytesOf(size);
    b.run(`pipeThrough() a TextDecoderStream, ${sizeLabel(size)}`, async () => {
      const reader = new Response(bytes).body
        .pipeThrough(new TextDecoderStream())
        .getReader();
      let length = 0;
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        length += value.length;
      }
      expectEqual(length, size, 'characters read');
    });
    b.run(
      `pipeThrough() an IdentityTransformStream, ${sizeLabel(size)}`,
      async () => {
        const reader = new Response(bytes).body
          .pipeThrough(new IdentityTransformStream())
          .getReader();
        expectEqual(await drainBytes(reader), size, 'bytes read');
      }
    );
  },
};
