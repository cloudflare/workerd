// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Request headers are read from a copy of the native headers until something needs storage of
// their own. These tests check that they behave exactly like ordinary Headers, including after
// the request that delivered them has ended.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';

let stashed = null;
let waitUntilSeen = null;

function sentHeaders() {
  const headers = new Headers();
  headers.append('X-Test', 'one');
  headers.append('x-dup', 'a');
  headers.append('X-Dup', 'b');
  headers.append('Accept', 'text/html');
  headers.append('accept', '*/*');
  headers.append('Set-Cookie', 'a=1');
  headers.append('set-cookie', 'b=2');
  return headers;
}

function describe(headers) {
  return {
    test: headers.get('x-test'),
    TEST: headers.get('X-TEST'),
    dup: headers.get('x-dup'),
    accept: headers.get('accept'),
    cookie: headers.get('set-cookie'),
    cookies: headers.getSetCookie(),
    hasTest: headers.has('x-test'),
    hasMissing: headers.has('x-missing'),
    entries: [...headers].filter(([k]) => k !== 'host' && k !== 'cf-ray'),
  };
}

const EXPECTED = {
  test: 'one',
  TEST: 'one',
  dup: 'a, b',
  accept: 'text/html, */*',
  cookie: 'a=1, b=2',
  cookies: ['a=1', 'b=2'],
  hasTest: true,
  hasMissing: false,
  entries: [
    ['accept', 'text/html, */*'],
    ['set-cookie', 'a=1'],
    ['set-cookie', 'b=2'],
    ['x-dup', 'a, b'],
    ['x-test', 'one'],
  ],
};

export default {
  async fetch(request, env, ctx) {
    const { pathname } = new URL(request.url);
    if (pathname === '/stash') {
      // Keep the request past the end of this call, both in a global and in a waitUntil task
      // that runs after the response has been sent.
      stashed = request;
      ctx.waitUntil(
        (async () => {
          await scheduler.wait(10);
          waitUntilSeen = describe(request.headers);
        })()
      );
      return new Response('stashed');
    }
    if (pathname === '/read') {
      return Response.json({
        stashed: describe(stashed.headers),
        waitUntilSeen,
      });
    }
    if (pathname === '/echo') {
      return Response.json(describe(request.headers));
    }
    if (pathname === '/proxy') {
      // Passes the request's headers on without anything reading them first.
      return env.SELF.fetch(new Request('http://example.com/echo', request));
    }
    if (pathname === '/copies') {
      // Reads first, so that a later read can't depend on iteration having materialized them.
      const before = {
        get: request.headers.get('x-dup'),
        has: request.headers.has('accept'),
        cookies: request.headers.getSetCookie(),
      };
      throws(() => request.headers.set('x-test', 'two'), TypeError);
      throws(() => request.headers.delete('x-test'), TypeError);

      const clone = request.clone();
      const copy = new Request(request);
      copy.headers.set('x-test', 'two');
      copy.headers.append('x-dup', 'c');
      copy.headers.delete('accept');
      const withHeaders = new Request(request, {
        headers: { 'x-only': 'yes' },
      });
      const fromHeaders = new Headers(request.headers);
      fromHeaders.append('Set-Cookie', 'c=3');
      const cloned = structuredClone(request.headers);

      return Response.json({
        before,
        original: describe(request.headers),
        clone: describe(clone.headers),
        copy: describe(copy.headers),
        withHeaders: [...withHeaders.headers],
        fromHeaders: describe(fromHeaders),
        cloned: describe(cloned),
        forEach: (() => {
          const seen = [];
          request.headers.forEach((v, k) => {
            if (k === 'x-test') seen.push([k, v]);
          });
          return seen;
        })(),
        keys: [...request.headers.keys()].filter((k) => k.startsWith('x-')),
        values: [...request.headers.values()].includes('a, b'),
      });
    }
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
    // Like any Response's headers, they can be changed, and the change sticks.
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

    // The Content-Type of a Blob body is trimmed, as it was when set on a Headers object.
    strictEqual(
      new Response(new Blob(['x'], { type: ' Text/Plain ' })).headers.get(
        'content-type'
      ),
      'text/plain'
    );
  },
};

export const requestHeadersAfterTheRequestEnds = {
  async test(ctrl, env) {
    const stash = await env.SELF.fetch('http://example.com/stash', {
      headers: sentHeaders(),
    });
    strictEqual(await stash.text(), 'stashed');
    // Let the waitUntil task run.
    await scheduler.wait(50);
    const read = await env.SELF.fetch('http://example.com/read');
    const { stashed, waitUntilSeen } = await read.json();
    deepStrictEqual(stashed, EXPECTED);
    deepStrictEqual(waitUntilSeen, EXPECTED);
  },
};

export const requestHeadersReadAndPassedOn = {
  async test(ctrl, env) {
    for (const path of ['/echo', '/proxy']) {
      const res = await env.SELF.fetch(`http://example.com${path}`, {
        headers: sentHeaders(),
      });
      deepStrictEqual(await res.json(), EXPECTED, path);
    }
  },
};

export const requestHeadersCopied = {
  async test(ctrl, env) {
    const res = await env.SELF.fetch('http://example.com/copies', {
      headers: sentHeaders(),
    });
    const result = await res.json();
    deepStrictEqual(result.before, {
      get: 'a, b',
      has: true,
      cookies: ['a=1', 'b=2'],
    });
    deepStrictEqual(result.original, EXPECTED);
    deepStrictEqual(result.clone, EXPECTED);
    deepStrictEqual(result.cloned, EXPECTED);
    deepStrictEqual(result.copy, {
      ...EXPECTED,
      test: 'two',
      TEST: 'two',
      dup: 'a, b, c',
      accept: null,
      entries: [
        ['set-cookie', 'a=1'],
        ['set-cookie', 'b=2'],
        ['x-dup', 'a, b, c'],
        ['x-test', 'two'],
      ],
    });
    deepStrictEqual(result.withHeaders, [['x-only', 'yes']]);
    deepStrictEqual(result.fromHeaders, {
      ...EXPECTED,
      cookie: 'a=1, b=2, c=3',
      cookies: ['a=1', 'b=2', 'c=3'],
      entries: [
        ['accept', 'text/html, */*'],
        ['set-cookie', 'a=1'],
        ['set-cookie', 'b=2'],
        ['set-cookie', 'c=3'],
        ['x-dup', 'a, b'],
        ['x-test', 'one'],
      ],
    });
    deepStrictEqual(result.forEach, [['x-test', 'one']]);
    deepStrictEqual(result.keys, ['x-dup', 'x-test']);
    strictEqual(result.values, true);
  },
};
