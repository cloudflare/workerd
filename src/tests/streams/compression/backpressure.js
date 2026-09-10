// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Write settlement: the codec push runs eagerly inside write(), so writes
// settle as soon as the codec has consumed the chunk — no read demand
// required. (Contrast the identity streams' rendezvous and the encoding
// streams' HWM-0 demand-driven transform.)

import { strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const desiredSizeAccounting = {
  async test() {
    // Divergence: the C++ writer's desiredSize is inert (always 1,
    // regardless of in-flight writes — the identity streams' default-HWM
    // behavior); TypeScript counts the in-flight chunk against the default
    // HWM of 1 and recovers once the eager push completes.
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    strictEqual(writer.desiredSize, 1);
    const writePromise = writer.write(new Uint8Array(100_000));
    strictEqual(writer.desiredSize, usingTsImpl ? 0 : 1);
    await writePromise;
    strictEqual(writer.desiredSize, 1);
    // ready is settled either way — eager settlement means no sustained
    // backpressure signal.
    await writer.ready;
    await writer.close();
  },
};

export const writesSettleWithoutReads = {
  async test() {
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    let settled = 0;
    const writes = [
      writer.write(new TextEncoder().encode('hello')).then(() => settled++),
      writer.write(new TextEncoder().encode('world')).then(() => settled++),
    ];
    await Promise.all(writes);
    strictEqual(settled, 2);
    await writer.close();

    // The buffered output is intact: gzip magic first.
    const reader = cs.readable.getReader();
    const { value } = await reader.read();
    strictEqual(value[0], 0x1f);
    strictEqual(value[1], 0x8b);
  },
};
