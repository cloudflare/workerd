// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TextEncoderStream's whole life per call: queue every string chunk and
// close, while the bytes are read to the end.

import { SHAPES, TEXT, expectEqual, pump, sizeLabel } from 'helpers';

export const encoder = {
  bench(b) {
    for (const [name, { char, bytesPerChar }] of Object.entries(TEXT)) {
      for (const [count, length] of SHAPES) {
        const chunk = char.repeat(length);
        b.run(
          `${name} ${count} x ${sizeLabel(length)} characters`,
          async () => {
            const bytes = await pump(
              new TextEncoderStream(),
              () => chunk,
              count,
              (value) => value.byteLength
            );
            expectEqual(bytes, count * length * bytesPerChar, 'bytes read');
          }
        );
      }
    }
  },
};
