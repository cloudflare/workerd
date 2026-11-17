// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Each attribute's setter, per backend. Only the href setter throws; the
// others leave the URL unchanged on a value they reject. The legacy
// backend's results are kj::Url's, quirks included: the facade passes
// them through unchanged.

import { throws, strictEqual } from 'node:assert';
import { urlStandard } from 'which-impl';

const kBase = 'http://u:p@a.com:81/p?q=1#h';

// [attribute, value, standard result, legacy result]. A result of
// `kBase` means the setter rejected the value.
const kCases = [
  ['href', 'http://x/y', 'http://x/y', 'http://x/y'],
  [
    'protocol',
    'https',
    'https://u:p@a.com:81/p?q=1#h',
    'https://u:p@a.com:81/p?q=1#h',
  ],
  [
    'protocol',
    'https:junk',
    'https://u:p@a.com:81/p?q=1#h',
    'https://u:p@a.com:81/p?q=1#h',
  ],
  ['protocol', 'file', kBase, 'file://u:p@a.com:81/p?q=1#h'],
  ['protocol', '1bad', kBase, kBase],
  [
    'username',
    'us er',
    'http://us%20er:p@a.com:81/p?q=1#h',
    'http://us%20er:p@a.com:81/p?q=1#h',
  ],
  [
    'password',
    'pa:ss',
    'http://u:pa%3Ass@a.com:81/p?q=1#h',
    'http://u:pa%3Ass@a.com:81/p?q=1#h',
  ],
  [
    'host',
    'b.com:99',
    'http://u:p@b.com:99/p?q=1#h',
    'http://u:p@b.com:99/p?q=1#h',
  ],
  [
    'host',
    'b.com',
    'http://u:p@b.com:81/p?q=1#h',
    'http://u:p@b.com:81/p?q=1#h',
  ],
  [
    'hostname',
    'c.com',
    'http://u:p@c.com:81/p?q=1#h',
    'http://u:p@c.com:81/p?q=1#h',
  ],
  ['hostname', '', kBase, 'http://u:p@:81/p?q=1#h'],
  [
    'port',
    '8080',
    'http://u:p@a.com:8080/p?q=1#h',
    'http://u:p@a.com:8080/p?q=1#h',
  ],
  ['port', '80', 'http://u:p@a.com/p?q=1#h', 'http://u:p@a.com/p?q=1#h'],
  ['port', 'abc', kBase, 'http://u:p@a.com:abc/p?q=1#h'],
  ['port', '99999', kBase, 'http://u:p@a.com:99999/p?q=1#h'],
  [
    'pathname',
    '/a b/%2F',
    'http://u:p@a.com:81/a%20b/%2F?q=1#h',
    'http://u:p@a.com:81/a%20b/%2F?q=1#h',
  ],
  ['pathname', '%zz', 'http://u:p@a.com:81/%zz?q=1#h', kBase],
  [
    'search',
    'q=a b',
    'http://u:p@a.com:81/p?q=a%20b#h',
    'http://u:p@a.com:81/p?q=a+b#h',
  ],
  ['search', '%zz', 'http://u:p@a.com:81/p?%zz#h', kBase],
  ['search', '', 'http://u:p@a.com:81/p#h', 'http://u:p@a.com:81/p#h'],
  [
    'hash',
    'h i',
    'http://u:p@a.com:81/p?q=1#h%20i',
    'http://u:p@a.com:81/p?q=1#h%20i',
  ],
  ['hash', '', 'http://u:p@a.com:81/p?q=1', 'http://u:p@a.com:81/p?q=1#'],
];

export const setterResults = {
  test() {
    for (const [name, value, standard, legacy] of kCases) {
      const url = new URL(kBase);
      url[name] = value;
      strictEqual(
        url.href,
        urlStandard ? standard : legacy,
        `${name} = ${value}`
      );
    }
  },
};

export const hrefSetterThrows = {
  test() {
    const url = new URL(kBase);
    throws(
      () => {
        url.href = 'nope';
      },
      {
        name: 'TypeError',
        message:
          "Failed to set the 'href' property on 'URL': the provided value is " +
          "not of type 'valid URL string'.",
      }
    );
    strictEqual(url.href, kBase);
  },
};

export const readonlyAttributes = {
  test() {
    const url = new URL(kBase);
    for (const name of ['origin', 'searchParams']) {
      const before = url[name];
      throws(() => {
        url[name] = 'x';
      }, TypeError);
      strictEqual(url[name], before);
    }
  },
};

export const setterValuesAreConverted = {
  test() {
    const url = new URL('http://a/p?q#h');
    url.search = undefined;
    url.hash = undefined;
    url.pathname = undefined;
    strictEqual(url.href, 'http://a/undefined?undefined#undefined');
    url.search = null;
    strictEqual(url.search, '?null');
    url.pathname = { toString: () => '/obj' };
    strictEqual(url.pathname, '/obj');
    throws(() => {
      url.search = Symbol();
    }, TypeError);
    strictEqual(url.search, '?null');
  },
};

// kj::Url accepts some hosts it cannot stringify; the getters then throw
// the friendly messages api/url.c++ translates.
export const legacyStringifyErrors = {
  test() {
    if (urlStandard) return;
    // kj::Url accepts a host it cannot serialize, and decodes a path's %2E
    // into a component it cannot serialize. Anything that serializes the
    // URL then throws, the protocol, username and password setters
    // included.
    const withBadHost = () => {
      const url = new URL('https://a/');
      url.host = 'a b';
      return url;
    };
    for (const [make, component] of [
      [withBadHost, 'hostname'],
      [() => new URL('https://a/%2E'), 'pathname'],
    ]) {
      const error = {
        name: 'TypeError',
        message: `Invalid ${component} when stringifying URL.`,
      };
      for (const serialize of [
        (url) => url.href,
        (url) => url.toString(),
        (url) => JSON.stringify(url),
        (url) => (url.protocol = 'http:'),
        (url) => (url.username = 'nemo'),
        (url) => (url.password = 'hunter12'),
      ]) {
        const url = make();
        throws(() => serialize(url), error);
      }
    }
  },
};
