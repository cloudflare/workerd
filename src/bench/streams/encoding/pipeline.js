// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Strings through TextEncoderStream and back through TextDecoderStream with
// pipeThrough(), read to the end.

import { SHAPES, expectEqual, sizeLabel } from 'helpers';

export const pipeline = {
  bench(b) {
    for (const [count, length] of SHAPES) {
      const chunk = 'a'.repeat(length);
      b.run(
        `encode then decode, ascii ${count} x ${sizeLabel(length)} characters`,
        async () => {
          const source = new ReadableStream({
            start(controller) {
              for (let i = 0; i < count; i++) controller.enqueue(chunk);
              controller.close();
            },
          });
          const reader = source
            .pipeThrough(new TextEncoderStream())
            .pipeThrough(new TextDecoderStream())
            .getReader();
          let total = 0;
          for (;;) {
            const { value, done } = await reader.read();
            if (done) break;
            total += value.length;
          }
          expectEqual(total, count * length, 'characters read');
        }
      );
    }
  },
};
