// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import { strictEqual, throws, ok as assert, match } from 'node:assert';
import {
  domainToASCII,
  domainToUnicode,
  URL as URLImpl,
  URLSearchParams as URLSearchParamsImpl,
  fileURLToPath,
  pathToFileURL,
} from 'node:url';

// Tests are taken from:
// https://github.com/nodejs/node/blob/321a14b36d6b3304aedfd183e12ddba35dc704bd/test/parallel/test-url-domain-ascii-unicode.js
export const testDomainAsciiUnicode = {
  test() {
    const domainWithASCII = [
      ['ıíd', 'xn--d-iga7r'],
      ['يٴ', 'xn--mhb8f'],
      ['www.ϧƽəʐ.com', 'www.xn--cja62apfr6c.com'],
      ['новини.com', 'xn--b1amarcd.com'],
      ['名がドメイン.com', 'xn--v8jxj3d1dzdz08w.com'],
      ['افغانستا.icom.museum', 'xn--mgbaal8b0b9b2b.icom.museum'],
      ['الجزائر.icom.fake', 'xn--lgbbat1ad8j.icom.fake'],
      ['भारत.org', 'xn--h2brj9c.org'],
      ['aaa', 'aaa'],
    ];

    for (const [domain, ascii] of domainWithASCII) {
      strictEqual(domainToASCII(domain), ascii);
      strictEqual(domainToUnicode(ascii), domain);
    }
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/321a14b36d6b3304aedfd183e12ddba35dc704bd/test/parallel/test-whatwg-url-custom-domainto.js
export const whatwgURLCustomDomainTo = {
  test() {
    {
      const expectedError = { code: 'ERR_MISSING_ARGS', name: 'TypeError' };
      throws(() => domainToASCII(), expectedError);
      throws(() => domainToUnicode(), expectedError);
      strictEqual(domainToASCII(undefined), 'undefined');
      strictEqual(domainToUnicode(undefined), 'undefined');
    }
  },
};

export const urlAndSearchParams = {
  test() {
    assert(URLImpl, 'URL should be exported from node:url');
    assert(
      URLSearchParamsImpl,
      'URLSearchParams should be exported by node:url'
    );
  },
};

export const getBuiltinModule = {
  async test() {
    const bim = process.getBuiltinModule('node:url');
    const url = await import('node:url');
    strictEqual(bim, url.default);
  },
};

// Ref: https://github.com/nodejs/node/blob/ddfef05f118e77cdb80a1f5971f076a03d221cb1/test/parallel/test-url-fileurltopath.js
export const testFileURLToPath = {
  async test() {
    // invalid arguments
    for (const arg of [null, undefined, 1, {}, true]) {
      throws(() => fileURLToPath(arg), {
        code: 'ERR_INVALID_ARG_TYPE',
      });
    }

    // input must be a file URL
    throws(() => fileURLToPath('https://a/b/c'), {
      code: 'ERR_INVALID_URL_SCHEME',
    });

    {
      // fileURLToPath with host
      const withHost = new URL('file://host/a');

      throws(() => fileURLToPath(withHost), {
        code: 'ERR_INVALID_FILE_URL_HOST',
      });
    }

    const windowsTestCases = [
      // Lowercase ascii alpha
      { path: 'C:\\foo', fileURL: 'file:///C:/foo' },
      // Uppercase ascii alpha
      { path: 'C:\\FOO', fileURL: 'file:///C:/FOO' },
      // dir
      { path: 'C:\\dir\\foo', fileURL: 'file:///C:/dir/foo' },
      // trailing separator
      { path: 'C:\\dir\\', fileURL: 'file:///C:/dir/' },
      // dot
      { path: 'C:\\foo.mjs', fileURL: 'file:///C:/foo.mjs' },
      // space
      { path: 'C:\\foo bar', fileURL: 'file:///C:/foo%20bar' },
      // question mark
      { path: 'C:\\foo?bar', fileURL: 'file:///C:/foo%3Fbar' },
      // number sign
      { path: 'C:\\foo#bar', fileURL: 'file:///C:/foo%23bar' },
      // ampersand
      { path: 'C:\\foo&bar', fileURL: 'file:///C:/foo&bar' },
      // equals
      { path: 'C:\\foo=bar', fileURL: 'file:///C:/foo=bar' },
      // colon
      { path: 'C:\\foo:bar', fileURL: 'file:///C:/foo:bar' },
      // semicolon
      { path: 'C:\\foo;bar', fileURL: 'file:///C:/foo;bar' },
      // percent
      { path: 'C:\\foo%bar', fileURL: 'file:///C:/foo%25bar' },
      // backslash
      { path: 'C:\\foo\\bar', fileURL: 'file:///C:/foo/bar' },
      // backspace
      { path: 'C:\\foo\bbar', fileURL: 'file:///C:/foo%08bar' },
      // tab
      { path: 'C:\\foo\tbar', fileURL: 'file:///C:/foo%09bar' },
      // newline
      { path: 'C:\\foo\nbar', fileURL: 'file:///C:/foo%0Abar' },
      // carriage return
      { path: 'C:\\foo\rbar', fileURL: 'file:///C:/foo%0Dbar' },
      // latin1
      { path: 'C:\\fóóbàr', fileURL: 'file:///C:/f%C3%B3%C3%B3b%C3%A0r' },
      // Euro sign (BMP code point)
      { path: 'C:\\€', fileURL: 'file:///C:/%E2%82%AC' },
      // Rocket emoji (non-BMP code point)
      { path: 'C:\\🚀', fileURL: 'file:///C:/%F0%9F%9A%80' },
      // UNC path (see https://docs.microsoft.com/en-us/archive/blogs/ie/file-uris-in-windows)
      {
        path: '\\\\nas\\My Docs\\File.doc',
        fileURL: 'file://nas/My%20Docs/File.doc',
      },
    ];

    const posixTestCases = [
      // Lowercase ascii alpha
      { path: '/foo', fileURL: 'file:///foo' },
      // Uppercase ascii alpha
      { path: '/FOO', fileURL: 'file:///FOO' },
      // dir
      { path: '/dir/foo', fileURL: 'file:///dir/foo' },
      // trailing separator
      { path: '/dir/', fileURL: 'file:///dir/' },
      // dot
      { path: '/foo.mjs', fileURL: 'file:///foo.mjs' },
      // space
      { path: '/foo bar', fileURL: 'file:///foo%20bar' },
      // question mark
      { path: '/foo?bar', fileURL: 'file:///foo%3Fbar' },
      // number sign
      { path: '/foo#bar', fileURL: 'file:///foo%23bar' },
      // ampersand
      { path: '/foo&bar', fileURL: 'file:///foo&bar' },
      // equals
      { path: '/foo=bar', fileURL: 'file:///foo=bar' },
      // colon
      { path: '/foo:bar', fileURL: 'file:///foo:bar' },
      // semicolon
      { path: '/foo;bar', fileURL: 'file:///foo;bar' },
      // percent
      { path: '/foo%bar', fileURL: 'file:///foo%25bar' },
      // backslash
      { path: '/foo\\bar', fileURL: 'file:///foo%5Cbar' },
      // backspace
      { path: '/foo\bbar', fileURL: 'file:///foo%08bar' },
      // tab
      { path: '/foo\tbar', fileURL: 'file:///foo%09bar' },
      // newline
      { path: '/foo\nbar', fileURL: 'file:///foo%0Abar' },
      // carriage return
      { path: '/foo\rbar', fileURL: 'file:///foo%0Dbar' },
      // latin1
      { path: '/fóóbàr', fileURL: 'file:///f%C3%B3%C3%B3b%C3%A0r' },
      // Euro sign (BMP code point)
      { path: '/€', fileURL: 'file:///%E2%82%AC' },
      // Rocket emoji (non-BMP code point)
      { path: '/🚀', fileURL: 'file:///%F0%9F%9A%80' },
    ];

    // fileURLToPath with windows path
    for (const { path, fileURL } of windowsTestCases) {
      const fromString = fileURLToPath(fileURL, { windows: true });
      strictEqual(fromString, path);
      const fromURL = fileURLToPath(new URL(fileURL), { windows: true });
      strictEqual(fromURL, path);
    }

    // fileURLToPath with posix path
    for (const { path, fileURL } of posixTestCases) {
      const fromString = fileURLToPath(fileURL, { windows: false });
      strictEqual(fromString, path);
      const fromURL = fileURLToPath(new URL(fileURL), { windows: false });
      strictEqual(fromURL, path);
    }

    {
      // options is null
      const whenNullActual = fileURLToPath(
        new URL(posixTestCases[0].fileURL),
        null
      );
      strictEqual(whenNullActual, posixTestCases[0].path);

      // default test cases
      for (const { path, fileURL } of posixTestCases) {
        const fromString = fileURLToPath(fileURL);
        strictEqual(fromString, path);
        const fromURL = fileURLToPath(new URL(fileURL));
        strictEqual(fromURL, path);
      }
    }
  },
};

// Ref: https://github.com/nodejs/node/blob/ddfef05f118e77cdb80a1f5971f076a03d221cb1/test/parallel/test-url-pathtofileurl.js
export const testPathToFileURL = {
  async test() {
    {
      const fileURL = pathToFileURL('test/').href;
      assert(fileURL.startsWith('file:///'));
      assert(fileURL.endsWith('/'));
    }

    {
      const fileURL = pathToFileURL('test\\').href;
      assert(fileURL.startsWith('file:///'));
      assert(fileURL.endsWith('%5C'));
    }

    {
      const fileURL = pathToFileURL('test/%').href;
      assert(fileURL.includes('%25'));
    }

    {
      // UNC path: \\server\share\resource
      // Missing server:
      throws(() => pathToFileURL('\\\\\\no-server', { windows: true }), {
        code: 'ERR_INVALID_ARG_VALUE',
      });

      // Missing share or resource:
      throws(() => pathToFileURL('\\\\host', { windows: true }), {
        code: 'ERR_INVALID_ARG_VALUE',
      });
      // Regression test for direct String.prototype.startsWith call
      throws(
        () =>
          pathToFileURL(
            ['\\\\', { [Symbol.toPrimitive]: () => 'blep\\blop' }],
            { windows: true }
          ),
        {
          code: 'ERR_INVALID_ARG_TYPE',
        }
      );
      throws(() => pathToFileURL(['\\\\', 'blep\\blop'], { windows: true }), {
        code: 'ERR_INVALID_ARG_TYPE',
      });
      throws(
        () =>
          pathToFileURL(
            {
              [Symbol.toPrimitive]: () => '\\\\blep\\blop',
            },
            { windows: true }
          ),
        {
          code: 'ERR_INVALID_ARG_TYPE',
        }
      );
    }

    {
      // UNC paths on posix are considered a single path that has backslashes:
      const fileURL = pathToFileURL('\\\\nas\\share\\path.txt', {
        windows: false,
      }).href;
      match(fileURL, /file:\/\/.+%5C%5Cnas%5Cshare%5Cpath\.txt$/);
    }

    const windowsTestCases = [
      // Lowercase ascii alpha
      { path: 'C:\\foo', expected: 'file:///C:/foo' },
      // Uppercase ascii alpha
      { path: 'C:\\FOO', expected: 'file:///C:/FOO' },
      // dir
      { path: 'C:\\dir\\foo', expected: 'file:///C:/dir/foo' },
      // trailing separator
      { path: 'C:\\dir\\', expected: 'file:///C:/dir/' },
      // dot
      { path: 'C:\\foo.mjs', expected: 'file:///C:/foo.mjs' },
      // space
      { path: 'C:\\foo bar', expected: 'file:///C:/foo%20bar' },
      // question mark
      { path: 'C:\\foo?bar', expected: 'file:///C:/foo%3Fbar' },
      // number sign
      { path: 'C:\\foo#bar', expected: 'file:///C:/foo%23bar' },
      // ampersand
      { path: 'C:\\foo&bar', expected: 'file:///C:/foo&bar' },
      // equals
      { path: 'C:\\foo=bar', expected: 'file:///C:/foo=bar' },
      // colon
      { path: 'C:\\foo:bar', expected: 'file:///C:/foo:bar' },
      // semicolon
      { path: 'C:\\foo;bar', expected: 'file:///C:/foo;bar' },
      // percent
      { path: 'C:\\foo%bar', expected: 'file:///C:/foo%25bar' },
      // backslash
      { path: 'C:\\foo\\bar', expected: 'file:///C:/foo/bar' },
      // backspace
      { path: 'C:\\foo\bbar', expected: 'file:///C:/foo%08bar' },
      // tab
      { path: 'C:\\foo\tbar', expected: 'file:///C:/foo%09bar' },
      // newline
      { path: 'C:\\foo\nbar', expected: 'file:///C:/foo%0Abar' },
      // carriage return
      { path: 'C:\\foo\rbar', expected: 'file:///C:/foo%0Dbar' },
      // latin1
      { path: 'C:\\fóóbàr', expected: 'file:///C:/f%C3%B3%C3%B3b%C3%A0r' },
      // Euro sign (BMP code point)
      { path: 'C:\\€', expected: 'file:///C:/%E2%82%AC' },
      // Rocket emoji (non-BMP code point)
      { path: 'C:\\🚀', expected: 'file:///C:/%F0%9F%9A%80' },
      // Local extended path
      {
        path: '\\\\?\\C:\\path\\to\\file.txt',
        expected: 'file:///C:/path/to/file.txt',
      },
      // UNC path (see https://docs.microsoft.com/en-us/archive/blogs/ie/file-uris-in-windows)
      {
        path: '\\\\nas\\My Docs\\File.doc',
        expected: 'file://nas/My%20Docs/File.doc',
      },
      // Extended UNC path
      {
        path: '\\\\?\\UNC\\server\\share\\folder\\file.txt',
        expected: 'file://server/share/folder/file.txt',
      },
    ];
    const posixTestCases = [
      // Lowercase ascii alpha
      { path: '/foo', expected: 'file:///foo' },
      // Uppercase ascii alpha
      { path: '/FOO', expected: 'file:///FOO' },
      // dir
      { path: '/dir/foo', expected: 'file:///dir/foo' },
      // trailing separator
      { path: '/dir/', expected: 'file:///dir/' },
      // dot
      { path: '/foo.mjs', expected: 'file:///foo.mjs' },
      // space
      { path: '/foo bar', expected: 'file:///foo%20bar' },
      // question mark
      { path: '/foo?bar', expected: 'file:///foo%3Fbar' },
      // number sign
      { path: '/foo#bar', expected: 'file:///foo%23bar' },
      // ampersand
      { path: '/foo&bar', expected: 'file:///foo&bar' },
      // equals
      { path: '/foo=bar', expected: 'file:///foo=bar' },
      // colon
      { path: '/foo:bar', expected: 'file:///foo:bar' },
      // semicolon
      { path: '/foo;bar', expected: 'file:///foo;bar' },
      // percent
      { path: '/foo%bar', expected: 'file:///foo%25bar' },
      // backslash
      { path: '/foo\\bar', expected: 'file:///foo%5Cbar' },
      // backspace
      { path: '/foo\bbar', expected: 'file:///foo%08bar' },
      // tab
      { path: '/foo\tbar', expected: 'file:///foo%09bar' },
      // newline
      { path: '/foo\nbar', expected: 'file:///foo%0Abar' },
      // carriage return
      { path: '/foo\rbar', expected: 'file:///foo%0Dbar' },
      // latin1
      { path: '/fóóbàr', expected: 'file:///f%C3%B3%C3%B3b%C3%A0r' },
      // Euro sign (BMP code point)
      { path: '/€', expected: 'file:///%E2%82%AC' },
      // Rocket emoji (non-BMP code point)
      { path: '/🚀', expected: 'file:///%F0%9F%9A%80' },
    ];

    for (const { path, expected } of windowsTestCases) {
      const actual = pathToFileURL(path, { windows: true }).href;
      strictEqual(actual, expected);
    }

    for (const { path, expected } of posixTestCases) {
      const actual = pathToFileURL(path, { windows: false }).href;
      strictEqual(actual, expected);
    }

    // Test for non-string parameter
    {
      for (const badPath of [
        undefined,
        null,
        true,
        42,
        42n,
        Symbol('42'),
        NaN,
        {},
        [],
        () => {},
        Promise.resolve('foo'),
        new Date(),
        new String('notPrimitive'),
        {
          toString() {
            return 'amObject';
          },
        },
        { [Symbol.toPrimitive]: (hint) => 'amObject' },
      ]) {
        throws(() => pathToFileURL(badPath), {
          code: 'ERR_INVALID_ARG_TYPE',
        });
      }
    }
  },
};
