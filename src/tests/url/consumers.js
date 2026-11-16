// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Runtime APIs that take a URL or URLSearchParams: Response and Request
// bodies, string-taking APIs, node:fs paths and URLPattern.

import { strictEqual, throws } from 'node:assert';
import fs from 'node:fs';

const kFormType = 'application/x-www-form-urlencoded;charset=UTF-8';

export const searchParamsBodies = {
  async test() {
    const params = new URLSearchParams([
      ['a', '1'],
      ['b c', '&'],
    ]);
    const response = new Response(params);
    strictEqual(response.headers.get('content-type'), kFormType);
    strictEqual(await response.text(), 'a=1&b+c=%26');

    const request = new Request('http://a/', { method: 'POST', body: params });
    strictEqual(request.headers.get('content-type'), kFormType);
    strictEqual(await request.text(), 'a=1&b+c=%26');

    // The body is the serialization, not the user's toString().
    class Sub extends URLSearchParams {
      toString() {
        return 'patched';
      }
    }
    strictEqual(await new Response(new Sub('x=1')).text(), 'x=1');

    // A URL's searchParams serializes its current list.
    const url = new URL('http://a/?q=1');
    url.searchParams.append('r', '2');
    strictEqual(await new Response(url.searchParams).text(), 'q=1&r=2');
  },
};

// A Proxy of a URLSearchParams or an object inheriting its prototype is not
// a URLSearchParams body; it falls through to a conversion that reaches a
// brand-checked member.
export const fakeSearchParamsBodies = {
  test() {
    for (const fake of [
      new Proxy(new URLSearchParams('a=1'), {}),
      Object.create(URLSearchParams.prototype),
    ]) {
      throws(() => new Response(fake), TypeError);
      throws(
        () => new Request('http://a/', { method: 'POST', body: fake }),
        TypeError
      );
    }
  },
};

export const urlsAsStrings = {
  test() {
    const url = new URL('http://a/p?q');
    strictEqual(new Request(url).url, 'http://a/p?q');
    strictEqual(Response.redirect(url).headers.get('location'), 'http://a/p?q');
    strictEqual(new URL('/r', url).href, 'http://a/r');
    strictEqual(`${url}`, 'http://a/p?q');
    strictEqual(JSON.stringify({ url }), '{"url":"http://a/p?q"}');
  },
};

export const nodeFsPaths = {
  test() {
    const path = new URL('file:///tmp/url-consumers.txt');
    fs.writeFileSync(path, 'hello');
    strictEqual(fs.readFileSync(path, 'utf8'), 'hello');
    strictEqual(fs.existsSync(path), true);
    fs.unlinkSync(path);
    strictEqual(fs.existsSync(path), false);
  },
};

// node:fs takes these for URLs (`instanceof URL` holds), and the runtime
// then rejects them.
export const fakeUrlFsPaths = {
  test() {
    const path = 'file:///tmp/url-consumers-fake.txt';
    for (const fake of [
      new Proxy(new URL(path), {}),
      Object.create(URL.prototype),
    ]) {
      throws(() => fs.writeFileSync(fake, 'hello'), TypeError);
      throws(() => fs.readFileSync(fake, 'utf8'), TypeError);
    }
    strictEqual(fs.existsSync(new URL(path)), false);
  },
};

export const urlPatternFromUrl = {
  test() {
    const pattern = new URLPattern(new URL('http://a.com/p'));
    strictEqual(pattern.protocol, 'http');
    strictEqual(pattern.hostname, 'a.com');
    strictEqual(pattern.pathname, '/p');
    strictEqual(pattern.test('http://a.com/p'), true);
    strictEqual(pattern.test(new URL('http://a.com/p')), true);
  },
};
