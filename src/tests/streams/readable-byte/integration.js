// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Byte streams across the fetch boundary via the SELF echo service.
// (Body-consumption mechanics for byte sources live in respond.js; this
// module covers full round-trips consumed with BYOB machinery.)

import { strictEqual, ok } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

// A JS byte source as a fetch body, echoed back and consumed with a
// BYOB reader.
export const byobRoundtrip = {
  async test(ctrl, env) {
    const rs = new ReadableStream({
      type: 'bytes',
      expectedLength: 10,
      pull(c) {
        c.enqueue(enc.encode('hellohello'));
        c.close();
      },
    });
    const resp = await env.SELF.fetch('http://example.org', {
      method: 'POST',
      body: rs,
    });
    const reader = resp.body.getReader({ mode: 'byob' });
    const parts = [];
    for (;;) {
      const { value, done } = await reader.read(new Uint8Array(4));
      if (done) break;
      parts.push(dec.decode(value));
    }
    strictEqual(parts.join(''), 'hellohello');
  },
};

// The echoed body supports readAtLeast across network chunk boundaries.
export const readAtLeastOnEchoedBody = {
  async test(ctrl, env) {
    const body = 'abcdefghij';
    const resp = await env.SELF.fetch('http://example.org', {
      method: 'POST',
      body,
    });
    const reader = resp.body.getReader({ mode: 'byob' });
    const result = await reader.readAtLeast(10, new Uint8Array(32));
    strictEqual(result.done, false);
    strictEqual(dec.decode(result.value), body);
  },
};

// bytes() on a Response wrapping a JS byte source returns a Uint8Array
// of the full payload (parity).
export const bytesMethodOnByteSource = {
  async test() {
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        c.enqueue(enc.encode('byte'));
        c.enqueue(enc.encode('wise'));
        c.close();
      },
    });
    const bytes = await new Response(rs).bytes();
    ok(bytes instanceof Uint8Array);
    strictEqual(dec.decode(bytes), 'bytewise');
  },
};
