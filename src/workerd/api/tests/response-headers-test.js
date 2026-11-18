// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A Response constructed without headers creates its Headers only when something uses them, and
// is sent with just the Content-Type its body implies. These cases check that every way of using
// such a Response sees that Content-Type, and that it is sent.

import { deepStrictEqual, strictEqual } from 'node:assert';

const TEXT = 'text/plain;charset=UTF-8';

export default {
  async fetch(request) {
    switch (new URL(request.url).pathname) {
      case '/text':
        return new Response('hello');
      case '/blob':
        return new Response(new Blob(['x'], { type: 'image/png' }));
      case '/none':
        return new Response(null);
      case '/changed': {
        const response = new Response('hello');
        response.headers.set('x-test', 'one');
        return response;
      }
      case '/clone':
        return new Response('hello').clone();
      case '/copy':
        return new Response('hello', new Response('ignored'));
    }
    return new Response('not found', { status: 404 });
  },
};

export const headersOfAnUnsentResponse = {
  async test() {
    const response = new Response('hello');
    strictEqual(response.headers, response.headers);
    deepStrictEqual([...response.headers], [['content-type', TEXT]]);
    deepStrictEqual([...new Response(null).headers], []);
    deepStrictEqual(
      [...new Response('hello').clone().headers],
      [['content-type', TEXT]]
    );
    strictEqual(await new Response('hello').text(), 'hello');
    const blob = new Blob(['x'], { type: 'image/png' });
    strictEqual((await new Response(blob).blob()).type, 'image/png');
  },
};

export const headersOfASentResponse = {
  async test(ctrl, env) {
    const expected = {
      '/text': [['content-type', TEXT]],
      '/blob': [['content-type', 'image/png']],
      '/none': [],
      '/changed': [
        ['content-type', TEXT],
        ['x-test', 'one'],
      ],
      '/clone': [['content-type', TEXT]],
      '/copy': [['content-type', TEXT]],
    };
    for (const [path, headers] of Object.entries(expected)) {
      const response = await env.SELF.fetch(`http://placeholder${path}`);
      // Content-Length comes from sending the body, not from the Response.
      const received = [...response.headers].filter(
        ([name]) => name !== 'content-length'
      );
      deepStrictEqual(received, headers, path);
      await response.arrayBuffer();
    }
  },
};
