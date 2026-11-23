// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A Response made without headers in its init doesn't create its Headers object until something
// asks for it. These tests check that what is sent, and what response.headers shows, is the same
// as if it had.

import { deepStrictEqual, strictEqual } from 'node:assert';

export default {
  async fetch(request, env, ctx) {
    const { pathname } = new URL(request.url);
    if (pathname === '/response/text') return new Response('hello');
    if (pathname === '/response/empty') return new Response();
    if (pathname === '/response/blob') {
      return new Response(new Blob(['x'], { type: ' Text/Plain ' }));
    }
    if (pathname === '/response/json') return Response.json({ a: 1 });
    if (pathname === '/response/json-status') {
      return Response.json({ a: 1 }, { status: 201 });
    }
    if (pathname === '/response/json-headers') {
      return Response.json({ a: 1 }, { headers: { 'x-a': 'b' } });
    }
    if (pathname === '/response/clone') return new Response('hello').clone();
    if (pathname === '/response/copy') {
      return new Response(null, new Response('hello'));
    }
    if (pathname === '/response/copy-body') {
      return new Response(new URLSearchParams('a=1'), new Response());
    }
    if (pathname === '/response/mutated') {
      const response = new Response('hello');
      response.headers.set('X-Late', 'yes');
      return response;
    }
    return new Response('not found', { status: 404 });
  },
};

function responseHeaders(response) {
  return [...response.headers].filter(
    ([k]) => k !== 'content-length' && k !== 'transfer-encoding'
  );
}

export const responseHeadersWithoutAHeadersObject = {
  async test(ctrl, env) {
    const cases = {
      text: [['content-type', 'text/plain;charset=UTF-8']],
      empty: [],
      blob: [['content-type', 'text/plain']],
      json: [['content-type', 'application/json']],
      'json-status': [['content-type', 'application/json']],
      'json-headers': [
        ['content-type', 'application/json'],
        ['x-a', 'b'],
      ],
      clone: [['content-type', 'text/plain;charset=UTF-8']],
      copy: [['content-type', 'text/plain;charset=UTF-8']],
      'copy-body': [
        ['content-type', 'application/x-www-form-urlencoded;charset=UTF-8'],
      ],
      mutated: [
        ['content-type', 'text/plain;charset=UTF-8'],
        ['x-late', 'yes'],
      ],
    };
    for (const [name, expected] of Object.entries(cases)) {
      const res = await env.SELF.fetch(`http://example.com/response/${name}`);
      deepStrictEqual(responseHeaders(res), expected, name);
      await res.arrayBuffer();
    }
  },
};

export const responseHeadersObject = {
  async test() {
    const response = new Response('hello');
    const headers = response.headers;
    strictEqual(response.headers, headers);
    deepStrictEqual(
      [...headers],
      [['content-type', 'text/plain;charset=UTF-8']]
    );
    // A body's Content-Type is normalized as Headers would normalize it.
    headers.set('x-a', 'b');
    strictEqual(response.headers.get('x-a'), 'b');
    strictEqual(response.headers, headers);

    deepStrictEqual([...new Response().headers], []);
    deepStrictEqual([...new Response(null, { status: 204 }).headers], []);
    deepStrictEqual([...Response.error().headers], []);
    deepStrictEqual(
      [...Response.json(null).headers],
      [['content-type', 'application/json']]
    );

    // Reading the body uses the pending Content-Type.
    strictEqual(
      (await new Response('x').blob()).type,
      'text/plain;charset=UTF-8'
    );
    strictEqual(
      (await new Response(new URLSearchParams('a=1')).formData()).get('a'),
      '1'
    );

    // Copies taken before the Headers object exists start out the same, and are independent.
    const original = new Response('hello');
    const clone = original.clone();
    const copy = new Response(null, original);
    clone.headers.set('x-clone', '1');
    deepStrictEqual(
      [...original.headers],
      [['content-type', 'text/plain;charset=UTF-8']]
    );
    deepStrictEqual(
      [...copy.headers],
      [['content-type', 'text/plain;charset=UTF-8']]
    );
    copy.headers.set('x-copy', '1');
    strictEqual(original.headers.get('x-copy'), null);
    strictEqual(original.headers.get('x-clone'), null);

    // A copied Content-Type wins over the one the new body implies.
    const form = new FormData();
    form.append('a', '1');
    strictEqual(
      new Response(form, new Response('hello')).headers.get('content-type'),
      'text/plain;charset=UTF-8'
    );
    strictEqual(
      new Response(form, new Response()).headers
        .get('content-type')
        .split(';')[0],
      'multipart/form-data'
    );

    // clone() copies the headers before the cf object, whose toJSON() runs while it's cloned.
    for (const init of [{}, { headers: { 'x-a': 'b' } }]) {
      const response = new Response('hello', { ...init, cf: {} });
      // Set after construction, which copies the init's cf object the same way.
      response.cf.toJSON = () => {
        response.headers.set('x-late', '1');
        return {};
      };
      const cloned = response.clone();
      strictEqual(response.headers.get('x-late'), '1');
      strictEqual(cloned.headers.get('x-late'), null);
      strictEqual(
        cloned.headers.get('content-type'),
        'text/plain;charset=UTF-8'
      );
    }

    // The Content-Type of a Blob body is trimmed, as it was when set on a Headers object.
    strictEqual(
      new Response(new Blob(['x'], { type: ' Text/Plain ' })).headers.get(
        'content-type'
      ),
      'text/plain'
    );
  },
};
