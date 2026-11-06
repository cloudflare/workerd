// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// test() and exec(): string inputs, parsed as URLs; init inputs, processed
// as URL components; baseURLs; and what each parser does when an input
// does not process.

import { throws, strictEqual, deepStrictEqual, ok } from 'node:assert';
import { usingTsImpl, urlPatternStandard } from 'which-impl';
import { kComponents } from 'helpers';

function inputs(result) {
  return kComponents.map((name) => result[name].input);
}

export const stringInputs = {
  test() {
    const pattern = new URLPattern({});
    deepStrictEqual(inputs(pattern.exec('https://u:p@a.com:8080/p/q?s=1#h')), [
      'https',
      'u',
      'p',
      'a.com',
      '8080',
      '/p/q',
      's=1',
      'h',
    ]);
    // Components are the parsed URL's: a default port is dropped, the
    // path normalized and percent-encoded.
    deepStrictEqual(inputs(pattern.exec('HTTP://A.com:80/a/../b c')), [
      'http',
      '',
      '',
      'a.com',
      '',
      '/b%20c',
      '',
      '',
    ]);
    deepStrictEqual(inputs(pattern.exec('/x?', 'https://b/y')), [
      'https',
      '',
      '',
      'b',
      '',
      '/x',
      '',
      '',
    ]);
    const path = new URLPattern({ pathname: '/books/:id' });
    strictEqual(path.test('https://a/books/1'), true);
    strictEqual(path.test('https://a/books/'), false);
    strictEqual(path.test('/books/1', 'https://a/'), true);
  },
};

export const unparseableStringInputs = {
  test() {
    for (const pattern of [
      new URLPattern({}),
      new URLPattern({ pathname: '*' }),
    ]) {
      // An unparseable baseURL, or an input that does not parse against
      // it, does not match.
      strictEqual(pattern.exec('/x', 'nope'), null);
      strictEqual(pattern.test('/x', 'nope'), false);
      strictEqual(pattern.exec('http://[', 'https://a/'), null);
      strictEqual(pattern.test('http://[', 'https://a/'), false);
    }
    // Ledger #1: with no baseURL, ada resolves an unparseable input
    // against an empty URL instead of failing.
    const pattern = new URLPattern({});
    const adaBug = urlPatternStandard && !usingTsImpl;
    strictEqual(pattern.test('nope'), adaBug);
    strictEqual(pattern.test(''), adaBug);
    strictEqual(pattern.test(5), adaBug);
    const result = pattern.exec('nope');
    if (adaBug) {
      deepStrictEqual(inputs(result), ['', '', '', '', '', '/nope', '', '']);
    } else {
      strictEqual(result, null);
    }
  },
};

export const initInputs = {
  test() {
    const pattern = new URLPattern({});
    deepStrictEqual(
      inputs(
        pattern.exec({
          protocol: 'HTTPS:',
          hostname: 'A.com',
          port: '443',
          pathname: '/a b',
          search: '?q',
          hash: '#h',
        })
      ),
      ['https', '', '', 'a.com', '', '/a%20b', 'q', 'h']
    );
    // A baseURL fills in the components before the first one given.
    deepStrictEqual(
      inputs(
        pattern.exec({ pathname: 'c', baseURL: 'https://u@b.com/a/b?q#h' })
      ),
      // The legacy parser takes the later components too.
      urlPatternStandard
        ? ['https', 'u', '', 'b.com', '', '/a/c', '', '']
        : ['https', 'u', '', 'b.com', '', '/a/c', 'q', 'h']
    );
    deepStrictEqual(inputs(pattern.exec()), Array(8).fill(''));
    deepStrictEqual(inputs(pattern.exec(null)), Array(8).fill(''));
    // A URL is an init, read through its getters.
    deepStrictEqual(inputs(pattern.exec(new URL('https://a/b?c#d'))), [
      'https',
      '',
      '',
      'a',
      '',
      '/b',
      'c',
      'd',
    ]);
  },
};

export const initInputsThatDoNotProcess = {
  test() {
    const pattern = new URLPattern({});
    for (const [init, legacyMessage] of [
      [{ protocol: 'a b' }, 'Invalid URL protocol component'],
      [{ hostname: 'a b' }, 'Invalid URL hostname component'],
      [{ port: 'x' }, 'Invalid URL port component'],
      [{ pathname: '/a', baseURL: 'nope' }, 'Invalid base URL.'],
    ]) {
      // The standard parser does not match; the legacy one throws.
      if (urlPatternStandard) {
        strictEqual(pattern.exec(init), null);
        strictEqual(pattern.test(init), false);
      } else {
        const error = { name: 'TypeError', message: legacyMessage };
        throws(() => pattern.exec(init), error);
        throws(() => pattern.test(init), error);
      }
    }
  },
};

export const initInputsWithABaseUrl = {
  test() {
    const pattern = new URLPattern({});
    for (const [method, standardMessage] of [
      ['exec', 'Failed to exec URLPattern'],
      ['test', 'Failed to test URLPattern'],
    ]) {
      throws(() => pattern[method]({}, 'https://a/'), {
        name: 'TypeError',
        message: urlPatternStandard
          ? standardMessage
          : 'A baseURL is not allowed when input is an object.',
      });
      // An object is an init, whatever its toString().
      const input = { toString: () => 'https://a/' };
      throws(() => pattern[method](input, 'https://a/'), {
        name: 'TypeError',
      });
    }
    // An explicit undefined is no baseURL.
    ok(pattern.exec({}, undefined) !== null);
  },
};

export const componentMatching = {
  test() {
    const pattern = new URLPattern({
      protocol: 'http{s}?',
      hostname: '*.example.com',
      pathname: '/api/:version/*',
      search: '',
    });
    strictEqual(pattern.test('https://a.example.com/api/v1/x/y'), true);
    strictEqual(pattern.test('http://b.example.com/api/v2/'), true);
    strictEqual(pattern.test('ftp://a.example.com/api/v1/x'), false);
    strictEqual(pattern.test('https://example.com/api/v1/x'), false);
    strictEqual(pattern.test('https://a.example.com/api/v1/x?q'), false);
    strictEqual(pattern.test('https://a.example.com/api/v1/x#h'), true);

    const exact = new URLPattern({ pathname: '/exact', hash: '' });
    strictEqual(exact.test({ pathname: '/exact' }), true);
    strictEqual(exact.test({ pathname: '/exact', hash: 'h' }), false);
    strictEqual(exact.test({ pathname: '/exact/' }), false);
  },
};

export const ignoreCase = {
  test() {
    const pattern = new URLPattern(
      { pathname: '/Books/:id', hostname: 'A.com' },
      { ignoreCase: true }
    );
    strictEqual(pattern.test('https://a.com/BOOKS/x'), true);
    strictEqual(pattern.exec('https://a.com/books/X').pathname.groups.id, 'X');
    strictEqual(
      new URLPattern({ pathname: '/Books' }).test({ pathname: '/books' }),
      false
    );
  },
};
