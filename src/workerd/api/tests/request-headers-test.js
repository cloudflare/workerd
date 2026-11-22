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
    return new Response('not found', { status: 404 });
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
