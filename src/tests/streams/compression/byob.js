// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The readable side is byte-capable in both implementations (the WHATWG
// spec describes a default stream here; the C++ pair has always been
// BYOB-readable and the TypeScript pair preserves that).

import { strictEqual, deepStrictEqual } from 'node:assert';

export const byobReadSupported = {
  async test() {
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    await writer.write(new TextEncoder().encode('byob'));
    await writer.close();
    const reader = cs.readable.getReader({ mode: 'byob' });
    const { value, done } = await reader.read(new Uint8Array(2));
    strictEqual(done, false);
    // gzip magic bytes fill the 2-byte destination exactly.
    deepStrictEqual([...value], [0x1f, 0x8b]);
    await reader.cancel();
  },
};
