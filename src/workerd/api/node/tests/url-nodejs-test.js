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

import { strictEqual, throws, ok as assert } from 'node:assert';
import {
  domainToASCII,
  domainToUnicode,
  URL as URLImpl,
  URLSearchParams as URLSearchParamsImpl,
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
