// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, rejects, strictEqual } from 'node:assert';

export const preventCancelKeepsNativeSourceUsable = {
  async test() {
    const stream = new FixedLengthStream(2);
    const writer = stream.writable.getWriter();
    const reason = new Error('destination failed');
    const destination = new WritableStream({
      write(chunk) {
        // A sink write proves the pipe's read completed. Pipe shutdown alone
        // does not acknowledge completion of an abandoned native read.
        deepStrictEqual(Array.from(chunk), [1]);
        throw reason;
      },
    });
    const piping = stream.readable.pipeTo(destination, { preventCancel: true });
    const pipeDone = rejects(piping, (error) => error === reason);
    await Promise.all([writer.write(new Uint8Array([1])), pipeDone]);
    strictEqual(stream.readable.locked, false);
    const reader = stream.readable.getReader();
    const next = reader.read();
    await writer.write(new Uint8Array([2]));
    const result = await next;
    strictEqual(result.done, false);
    deepStrictEqual(result.value, new Uint8Array([2]));
    const eof = reader.read();
    await writer.close();
    strictEqual((await eof).done, true);
  },
};

export const repeatedBodyTransferKeepsBytes = {
  async test() {
    let request = new Request('https://example.com', {
      method: 'POST',
      body: 'healthy',
    });
    for (let i = 0; i < 10000; i++) request = new Request(request);
    strictEqual(await request.text(), 'healthy');
  },
};
