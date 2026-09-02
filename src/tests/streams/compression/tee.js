// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// tee() on the compressed readable: both branches observe identical bytes;
// cancelling one branch leaves the survivor draining to EOF.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const enc = new TextEncoder();

async function drainBytes(readable) {
  const bytes = [];
  for await (const chunk of readable) {
    bytes.push(...chunk);
  }
  return bytes;
}

export const teeBothBranchesIdentical = {
  async test() {
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    await writer.write(enc.encode('tee me'));
    await writer.close();
    const [a, b] = cs.readable.tee();
    deepStrictEqual(await drainBytes(a), await drainBytes(b));
  },
};

export const cancelOneBranchSurvivorDrains = {
  async test() {
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    await writer.write(enc.encode('survivor'));
    await writer.close();
    const [a, b] = cs.readable.tee();
    // The single-branch cancel promise carries the identity suite's ledger
    // #13 divergence: C++ resolves it immediately, TypeScript uses the
    // WHATWG shared composite promise that settles only when BOTH branches
    // cancel — so it is only awaited under C++.
    const cancelPromise = b.cancel(new Error('done with b'));
    cancelPromise.catch(() => {});
    if (!usingTsImpl) {
      await cancelPromise;
    }
    const bytes = await drainBytes(a);
    strictEqual(bytes.length > 0, true);
    strictEqual(bytes[0], 0x1f);
  },
};
