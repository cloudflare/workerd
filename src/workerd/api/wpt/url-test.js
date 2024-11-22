// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { run } from 'harness';

export const idnaTestV2Window = run('IdnaTestV2.window.js');
export const historical = run('historical.any.js', {
  expectedFailures: ["Setting URL's href attribute and base URLs"],
});
// TODO(soon): Implement `globalThis.document`
export const javascriptUrlsWindow = run('javascript-urls.window.js', {
  expectedFailures: [
    'javascript: URL that fails to parse due to invalid host',
    'javascript: URL that fails to parse due to invalid host and has a U+0009 in scheme',
    'javascript: URL without an opaque path',
    'javascript: URL containing a JavaScript string split over path and query',
    'javascript: URL containing a JavaScript string split over path and query and has a U+000A in scheme',
  ],
});
// TODO(soon): Implement `async_test`
// export const percentEncodingWindow = run('percent-encoding.window.js');
// TODO(soon): Implement document.
// export const toAsciiWindow = run('toascii.window.js');
export const urlConstructorAny = run('url-constructor.any.js', {
  expectedFailures: [
    'Parsing: <http://example.com/\uD800\uD801\uDFFE\uDFFF\uFDD0\uFDCF\uFDEF\uFDF0\uFFFE\uFFFF?\uD800\uD801\uDFFE\uDFFF\uFDD0\uFDCF\uFDEF\uFDF0\uFFFE\uFFFF> without base',
  ],
});
export const urlOriginAny = run('url-origin.any.js');
export const urlSearchParamsAny = run('url-searchparams.any.js');
// TODO(soon): Implement promise_test
// export const urlSettersAAreaWindow = run('url-setters-a-area.window.js');
export const urlSettersStripping = run('url-setters-stripping.any.js');
export const urlSettersAny = run('url-setters.any.js');
export const urlStaticsCanParse = run('url-statics-canparse.any.js');
export const urlStaticsParse = run('url-statics-parse.any.js');
export const urlToJson = run('url-tojson.any.js');
export const urlencodedParser = run('urlencoded-parser.any.js', {
  expectedFailures: [
    'request.formData() with input: test',
    'response.formData() with input: test',
    'request.formData() with input: \uFEFFtest=\uFEFF',
    'response.formData() with input: \uFEFFtest=\uFEFF',
    'request.formData() with input: %EF%BB%BFtest=%EF%BB%BF',
    'response.formData() with input: %EF%BB%BFtest=%EF%BB%BF',
    'request.formData() with input: %EF%BF%BF=%EF%BF%BF',
    'response.formData() with input: %EF%BF%BF=%EF%BF%BF',
    'request.formData() with input: %FE%FF',
    'response.formData() with input: %FE%FF',
    'request.formData() with input: %FF%FE',
    'response.formData() with input: %FF%FE',
    'request.formData() with input: †&†=x',
    'response.formData() with input: †&†=x',
    'request.formData() with input: %C2',
    'response.formData() with input: %C2',
    'request.formData() with input: %C2x',
    'response.formData() with input: %C2x',
    'request.formData() with input: _charset_=windows-1252&test=%C2x',
    'response.formData() with input: _charset_=windows-1252&test=%C2x',
    'request.formData() with input: ',
    'response.formData() with input: ',
    'request.formData() with input: a',
    'response.formData() with input: a',
    'request.formData() with input: a=b',
    'response.formData() with input: a=b',
    'request.formData() with input: a=',
    'response.formData() with input: a=',
    'request.formData() with input: =b',
    'response.formData() with input: =b',
    'request.formData() with input: &',
    'response.formData() with input: &',
    'request.formData() with input: &a',
    'response.formData() with input: &a',
    'request.formData() with input: a&',
    'response.formData() with input: a&',
    'request.formData() with input: a&a',
    'response.formData() with input: a&a',
    'request.formData() with input: a&b&c',
    'response.formData() with input: a&b&c',
    'request.formData() with input: a=b&c=d',
    'response.formData() with input: a=b&c=d',
    'request.formData() with input: a=b&c=d&',
    'response.formData() with input: a=b&c=d&',
    'request.formData() with input: &&&a=b&&&&c=d&',
    'response.formData() with input: &&&a=b&&&&c=d&',
    'request.formData() with input: a=a&a=b&a=c',
    'response.formData() with input: a=a&a=b&a=c',
    'request.formData() with input: a==a',
    'response.formData() with input: a==a',
    'request.formData() with input: a=a+b+c+d',
    'response.formData() with input: a=a+b+c+d',
    'request.formData() with input: %=a',
    'response.formData() with input: %=a',
    'request.formData() with input: %a=a',
    'response.formData() with input: %a=a',
    'request.formData() with input: %a_=a',
    'response.formData() with input: %a_=a',
    'request.formData() with input: %61=a',
    'response.formData() with input: %61=a',
    'request.formData() with input: %61+%4d%4D=',
    'response.formData() with input: %61+%4d%4D=',
    'request.formData() with input: id=0&value=%',
    'response.formData() with input: id=0&value=%',
    'request.formData() with input: b=%2sf%2a',
    'response.formData() with input: b=%2sf%2a',
    'request.formData() with input: b=%2%2af%2a',
    'response.formData() with input: b=%2%2af%2a',
    'request.formData() with input: b=%%2a',
    'response.formData() with input: b=%%2a',
  ],
});
export const urlSearchParamsAppend = run('urlsearchparams-append.any.js');
export const urlSearchParamsConstructor = run(
  'urlsearchparams-constructor.any.js',
  {
    expectedFailures: [
      'URLSearchParams constructor, DOMException as argument',
      'Construct with 2 unpaired surrogates (no trailing)',
      'Construct with 3 unpaired surrogates (no leading)',
      'Construct with object with NULL, non-ASCII, and surrogate keys',
    ],
  }
);
export const urlSearchParamsDelete = run('urlsearchparams-delete.any.js');
export const urlSearchParamsForEach = run('urlsearchparams-foreach.any.js', {
  skippedTests: ['For-of Check'],
});
export const urlSearchParamsGetAny = run('urlsearchparams-get.any.js');
export const urlSearchParamsGetAll = run('urlsearchparams-getall.any.js');
export const urlSearchParamsHas = run('urlsearchparams-has.any.js');
export const urlSearchParamsSet = run('urlsearchparams-set.any.js');
export const urlSearchParamsSize = run('urlsearchparams-size.any.js');
export const urlSearchParamsSort = run('urlsearchparams-sort.any.js', {
  expectedFailures: ['Parse and sort: ﬃ&🌈', 'URL parse and sort: ﬃ&🌈'],
});
export const urlSearchParamsStringifier = run(
  'urlsearchparams-stringifier.any.js'
);
