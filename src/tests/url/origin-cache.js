// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `origin` after construction and after every setter. The TypeScript
// implementation caches the ada backend's origin and clears it in the
// setters that can change it; these tests read `origin` before each set,
// so a stale cache would show as a mismatch with a freshly parsed URL.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { urlStandard } from 'which-impl';

export const originValues = {
  test() {
    const cases = [
      ['http://a.com:81/x', 'http://a.com:81'],
      ['ws://a:80/', 'ws://a'],
      ['http://[::1]:8080/', 'http://[::1]:8080'],
      ['file:///x', 'null'],
    ];
    if (urlStandard) {
      cases.push(
        ['blob:http://a.com:81/x', 'http://a.com:81'],
        ['blob:https://a.com/x', 'https://a.com'],
        ['blob:file:///x', 'null'],
        ['blob:ftp://a/x', 'null'],
        ['blob:nope', 'null'],
        ['data:,x', 'null'],
        ['http://\u2603.com/', 'http://xn--n3h.com']
      );
    }
    deepStrictEqual(
      cases.map(([input]) => new URL(input).origin),
      cases.map(([, origin]) => origin)
    );
  },
};

const kStarts = urlStandard
  ? [
      'http://u:p@a.com:81/p?q=1#h',
      'https://a.com/',
      'blob:http://a.com:81/x',
      'blob:https://a.com/x',
      'blob:nope',
      'file:///x',
      'foo://h/p',
    ]
  : ['http://u:p@a.com:81/p?q=1#h', 'https://a.com/', 'file:///x'];

const kValues = {
  href: ['http://b.com:82/', 'blob:https://c.com/y', 'file:///z'],
  protocol: ['https', 'ws', 'file', 'blob', 'foo'],
  username: ['user'],
  password: ['pass'],
  host: ['b.com:99', 'b.com', 'b.com:443'],
  hostname: ['c.com', 'xn--n3h.com'],
  port: ['8080', '443', '80', ''],
  pathname: ['/a', 'http://evil.com/', 'https://evil.com/x'],
  search: ['?x=1'],
  hash: ['#y'],
};

export const originFollowsEverySetter = {
  test() {
    for (const start of kStarts) {
      for (const [name, values] of Object.entries(kValues)) {
        for (const value of values) {
          const url = new URL(start);
          // Populate any cache first.
          void url.origin;
          try {
            url[name] = value;
          } catch {
            continue;
          }
          let href;
          try {
            href = url.href;
          } catch {
            // The legacy backend may not stringify the result.
            continue;
          }
          strictEqual(
            url.origin,
            new URL(href).origin,
            `${start}: ${name} = ${value}`
          );
        }
      }
    }
  },
};

export const originAfterSearchParamsMutation = {
  test() {
    const url = new URL(
      urlStandard ? 'blob:https://a.com/x?q' : 'https://a.com/x?q'
    );
    const before = url.origin;
    url.searchParams.append('k', 'v');
    strictEqual(url.origin, before);
    strictEqual(url.origin, new URL(url.href).origin);
  },
};
