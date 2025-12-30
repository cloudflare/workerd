// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

// Test Response body methods with JS-backed BYOB ReadableStream
export const responseBodyMethodsJsByob = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    {
      const rs = new ReadableStream({
        type: 'bytes',
        async pull(c) {
          if (c.byobRequest) {
            enc.encodeInto('hello', c.byobRequest.view);
            c.byobRequest.respond(5);
            c.close();
          }
        },
      });

      const resp = new Response(rs);

      strictEqual(dec.decode(await resp.arrayBuffer()), 'hello');
    }

    {
      const rs = new ReadableStream({
        type: 'bytes',
        async pull(c) {
          if (c.byobRequest) {
            enc.encodeInto('hello', c.byobRequest.view);
            c.byobRequest.respond(5);
            c.close();
          }
        },
      });

      const resp = new Response(rs);

      strictEqual(await resp.text(), 'hello');
    }
  },
};

// Test Request body methods with JS-backed ReadableStream
export const requestBodyMethodsJsByob = {
  async test() {
    const enc = new TextEncoder();
    const wrapped = new ReadableStream({
      type: 'bytes',
      async pull(c) {
        c.enqueue(enc.encode('hello'));
        c.close();
      },
    });

    const req = new Request('http://example.com', {
      method: 'POST',
      body: wrapped,
    });
    const text = await req.text();

    strictEqual(text, 'hello');
  },
};
