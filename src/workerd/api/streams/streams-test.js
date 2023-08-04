// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export const partiallyReadStream= {
  async test(ctrl, env, ctx) {
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      type: 'bytes',
      start(controller) {
        controller.enqueue(enc.encode('hello'));
        controller.enqueue(enc.encode('world'));
        controller.close();
      }
    });
    const reader = rs.getReader({ mode: 'byob' });
    await reader.read(new Uint8Array(5));
    reader.releaseLock();

    // Should not throw!
    await env.KV.put("key", rs);
  }
};

export const arrayBufferOfReadable = {
  async test() {
    const cs = new CompressionStream("gzip");
    const cw = cs.writable.getWriter();
    await cw.write(new TextEncoder().encode("0123456789".repeat(1000)));
    await cw.close();
    const data = await new Response(cs.readable).arrayBuffer();
    assert.equal(66, data.byteLength);

    const ds = new DecompressionStream("gzip");
    const dw = ds.writable.getWriter();
    await dw.write(data);
    await dw.close();

    const read = await new Response(ds.readable).arrayBuffer();
    assert.equal(10_000, read.byteLength);
  }
}
