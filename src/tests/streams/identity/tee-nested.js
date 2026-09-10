// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// tee() of a tee() branch: nesting must preserve content, ordering, and
// close propagation through every leaf, and must not deadlock when only a
// single leaf is consumed.

import { strictEqual } from 'node:assert';

async function readAllText(readable) {
  const reader = readable.getReader();
  const dec = new TextDecoder();
  let text = '';
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    text += dec.decode(value, { stream: true });
  }
  return text + dec.decode();
}

export const teeOfTeeBranchDeliversToAllLeaves = {
  async test() {
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [a, b] = its.readable.tee();
    const [c, d] = a.tee();
    // Multiple chunks, so leaf content also proves cross-branch ordering.
    const writePromise = (async () => {
      await writer.write(new TextEncoder().encode('nested '));
      await writer.write(new TextEncoder().encode('tee'));
      await writer.close();
    })();
    const [textB, textC, textD] = await Promise.all([
      readAllText(b),
      readAllText(c),
      readAllText(d),
    ]);
    strictEqual(textB, 'nested tee');
    strictEqual(textC, 'nested tee');
    strictEqual(textD, 'nested tee');
    await writePromise;
  },
};

export const nestedTeeSingleLeafReadDoesNotHang = {
  async test() {
    // Reading a single leaf two tees deep must be enough demand to drive
    // the writer; the unread branches buffer.
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [a, _b] = its.readable.tee();
    const [c, _d] = a.tee();
    const writePromise = writer.write(new TextEncoder().encode('leaf'));
    const closePromise = writer.close();
    strictEqual(await readAllText(c), 'leaf');
    await writePromise;
    await closePromise;
  },
};
