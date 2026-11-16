// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';
import { inspect } from 'node:util';
import { onPrototype } from 'which-impl';

export const inspectInstances = {
  test() {
    const blob = new Blob(['ab'], { type: 'x/y' });
    const file = new File(['ab'], 'f.txt', { type: 'x/y', lastModified: 5 });
    if (onPrototype) {
      strictEqual(inspect(blob), "Blob { size: 2, type: 'x/y' }");
      strictEqual(
        inspect(file),
        "File { name: 'f.txt', lastModified: 5, size: 2, type: 'x/y' }"
      );
    } else {
      strictEqual(inspect(blob), "Blob { type: 'x/y', size: 2 }");
      strictEqual(
        inspect(file),
        "File { lastModified: 5, name: 'f.txt', type: 'x/y', size: 2 }"
      );
    }
  },
};
