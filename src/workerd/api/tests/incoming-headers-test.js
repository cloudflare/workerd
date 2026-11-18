// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The Headers of an incoming request or of a subrequest's response parse the received headers
// only when something first uses them. These cases check that every way of using them sees the
// headers as received, and that copies taken before then are independent of each other.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';

const SENT = [
  ['X-Test', 'one'],
  ['Accept', 'text/plain'],
  ['x-test', 'two'],
  ['X-Other', 'z'],
];

export default {
  async fetch(request, env) {
    switch (new URL(request.url).pathname) {
      case '/echo':
        return Response.json([...request.headers]);
      case '/copies': {
        // Nothing has read the headers before these copies are taken.
        const copy = new Headers(request.headers);
        const forwarded = new Request(request);
        const cloned = request.clone();
        copy.set('x-test', 'copy');
        forwarded.headers.delete('x-test');
        return Response.json({
          original: [...request.headers],
          copy: [...copy],
          forwarded: [...forwarded.headers],
          cloned: [...cloned.headers],
        });
      }
      case '/forward':
        // The request goes on without anything reading its headers here.
        return env.SELF.fetch(new Request('http://placeholder/echo', request));
      case '/immutable':
        throws(() => request.headers.set('x-test', 'three'), TypeError);
        return Response.json(request.headers.get('x-test'));
      case '/cookies': {
        const headers = new Headers();
        headers.append('Set-Cookie', 'a=1');
        headers.append('Set-Cookie', 'b=2');
        headers.append('X-Dup', 'x');
        headers.append('x-dup', 'y');
        return new Response('', { headers });
      }
      case '/proxy-cookies':
        // The subrequest's response goes on without anything reading its headers here.
        return env.SELF.fetch('http://placeholder/cookies');
      default:
        return new Response('not found', { status: 404 });
    }
  },
};

const expected = [...new Headers(SENT)];

async function get(env, path, init = { headers: SENT }) {
  return await env.SELF.fetch(`http://placeholder${path}`, init);
}

export const echo = {
  async test(ctrl, env) {
    deepStrictEqual(await (await get(env, '/echo')).json(), expected);
  },
};

export const echoWithBody = {
  async test(ctrl, env) {
    const response = await get(env, '/echo', {
      method: 'POST',
      headers: SENT,
      body: 'abc',
    });
    deepStrictEqual(await response.json(), [
      ...new Headers([
        ...SENT,
        ['Content-Length', '3'],
        ['Content-Type', 'text/plain;charset=UTF-8'],
      ]),
    ]);
  },
};

export const copies = {
  async test(ctrl, env) {
    const result = await (await get(env, '/copies')).json();
    deepStrictEqual(result.original, expected);
    deepStrictEqual(result.cloned, expected);
    deepStrictEqual(
      result.copy,
      expected.map(([name, value]) => [
        name,
        name === 'x-test' ? 'copy' : value,
      ])
    );
    deepStrictEqual(
      result.forwarded,
      expected.filter(([name]) => name !== 'x-test')
    );
  },
};

export const forward = {
  async test(ctrl, env) {
    deepStrictEqual(await (await get(env, '/forward')).json(), expected);
  },
};

export const immutable = {
  async test(ctrl, env) {
    strictEqual(await (await get(env, '/immutable')).json(), 'one, two');
  },
};

export const manyHeaders = {
  async test(ctrl, env) {
    // More headers than the copy collects on the stack.
    const many = Array.from({ length: 50 }, (_, i) => [`x-many-${i}`, `${i}`]);
    for (const path of ['/echo', '/copies', '/forward']) {
      const response = await get(env, path, { headers: many });
      const result = await response.json();
      deepStrictEqual(
        path === '/copies' ? result.cloned : result,
        [...new Headers(many)],
        path
      );
    }
  },
};

export const responseHeaders = {
  async test(ctrl, env) {
    for (const path of ['/cookies', '/proxy-cookies']) {
      const response = await get(env, path, {});
      deepStrictEqual(response.headers.getSetCookie(), ['a=1', 'b=2']);
      strictEqual(response.headers.get('x-dup'), 'x, y');
    }
  },
};
