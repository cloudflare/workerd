// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The Content-Length carried by a fetch body built from a FixedLengthStream's
// readable side, and by its tee branches, observed through the SELF echo
// service.
//
// PARITY: the readable, and branches teed before any read, carry the full
// length.
//
// DIVERGENCE (ledger #26): branches teed after a read. C++ carries what each
// branch has left. TypeScript sends them chunked: its branches share the
// readable's controller, which knows the declared total but not what is left
// once something has been read (the readable suite's ledger #35).

import { deepStrictEqual, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const enc = new TextEncoder();

// A FixedLengthStream of 10 bytes written in two chunks, with the writes and
// the close still pending.
function tenByteFixedLengthStream() {
  const { readable, writable } = new FixedLengthStream(10);
  const writer = writable.getWriter();
  const written = Promise.all([
    writer.write(enc.encode('01234')),
    writer.write(enc.encode('56789')),
    writer.close(),
  ]);
  return { readable, written };
}

// Posts the body through SELF, and returns the Content-Length it carried and
// the body echoed back.
async function post(env, body) {
  const resp = await env.SELF.fetch('http://example.org', {
    method: 'POST',
    body,
  });
  const text = await resp.text();
  return [resp.headers.get('observed-content-length'), text];
}

export const fixedLengthBodyContentLength = {
  async test(ctrl, env) {
    {
      const { readable, written } = tenByteFixedLengthStream();
      deepStrictEqual(await post(env, readable), ['10', '0123456789']);
      await written;
    }
    {
      const { readable, written } = tenByteFixedLengthStream();
      for (const branch of readable.tee()) {
        deepStrictEqual(await post(env, branch), ['10', '0123456789']);
      }
      await written;
    }
  },
};

export const fixedLengthTeeAfterReadContentLength = {
  async test(ctrl, env) {
    const { readable, written } = tenByteFixedLengthStream();
    const reader = readable.getReader();
    const { value } = await reader.read();
    reader.releaseLock();
    strictEqual(new TextDecoder().decode(value), '01234');
    const length = usingTsImpl ? 'null' : '5';
    for (const branch of readable.tee()) {
      deepStrictEqual(await post(env, branch), [length, '56789']);
    }
    await written;
  },
};
