// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// tee() on a decoder stream's readable: both branches observe the decoded
// content, one branch's demand is enough to drive the writer, and both see
// EOF after close.

import { strictEqual } from 'node:assert';

export const decoderTeeBothBranches = {
  async test() {
    const tds = new TextDecoderStream();
    const writer = tds.writable.getWriter();
    const [a, b] = tds.readable.tee();
    const readerA = a.getReader();
    const readerB = b.getReader();

    const writePromise = writer.write(new TextEncoder().encode('t'));
    strictEqual((await readerA.read()).value, 't');
    await writePromise;
    strictEqual((await readerB.read()).value, 't');

    await writer.close();
    strictEqual((await readerA.read()).done, true);
    strictEqual((await readerB.read()).done, true);
  },
};
