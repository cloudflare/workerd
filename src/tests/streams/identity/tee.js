// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// tee() on the readable side of identity streams: both branches observe the
// full content.

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

export const teeIdentityBothBranches = {
  async test() {
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [left, right] = its.readable.tee();
    const writePromise = (async () => {
      await writer.write(new TextEncoder().encode('foo bar baz'));
      await writer.close();
    })();
    const [leftText, rightText] = await Promise.all([
      readAllText(left),
      readAllText(right),
    ]);
    strictEqual(leftText, 'foo bar baz');
    strictEqual(rightText, 'foo bar baz');
    await writePromise;
  },
};

export const teeFixedLengthBothBranches = {
  async test() {
    const fls = new FixedLengthStream(11);
    const writer = fls.writable.getWriter();
    const [left, right] = fls.readable.tee();
    const writePromise = (async () => {
      await writer.write(new TextEncoder().encode('foo bar baz'));
      await writer.close();
    })();
    const [leftText, rightText] = await Promise.all([
      readAllText(left),
      readAllText(right),
    ]);
    strictEqual(leftText, 'foo bar baz');
    strictEqual(rightText, 'foo bar baz');
    await writePromise;
  },
};

export const teeSingleBranchReadDoesNotHang = {
  async test() {
    // Reading only one branch to completion must not deadlock even though
    // the other branch is never read.
    const fls = new FixedLengthStream(11);
    const writer = fls.writable.getWriter();
    const writePromise = writer.write(new TextEncoder().encode('foo bar baz'));
    const closePromise = writer.close();
    const [left] = fls.readable.tee();
    strictEqual(await readAllText(left), 'foo bar baz');
    await writePromise;
    await closePromise;
  },
};
