// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'wpt:harness';

export default {
  'IdnaTestV2.window.js': {},
  'historical.any.js': {
    comment: 'Fix this eventually',
    expectedFailures: [
      'URL: no structured serialize/deserialize support',
      'URLSearchParams: no structured serialize/deserialize support',
    ],
  },
  'idlharness.any.js': {
    comment: 'Does not contain any relevant tests',
    skipAllTests: true,
  },
  'javascript-urls.window.js': {
    comment: 'Implement `globalThis.document`',
    expectedFailures: [
      'javascript: URL that fails to parse due to invalid host',
      'javascript: URL that fails to parse due to invalid host and has a U+0009 in scheme',
      'javascript: URL without an opaque path',
      'javascript: URL containing a JavaScript string split over path and query',
      'javascript: URL containing a JavaScript string split over path and query and has a U+000A in scheme',
    ],
  },
  'percent-encoding.window.js': {
    comment:
      'Implement test code modification feature to allow running this test without document',
    skipAllTests: true,
  },
  'toascii.window.js': {
    comment: 'Implement document',
    skipAllTests: true,
  },
  'url-constructor.any.js': {
    comment: 'Fix this eventually',
    expectedFailures: [
      'Parsing: <http://example.com/\uD800\uD801\uDFFE\uDFFF\uFDD0\uFDCF\uFDEF\uFDF0\uFFFE\uFFFF?\uD800\uD801\uDFFE\uDFFF\uFDD0\uFDCF\uFDEF\uFDF0\uFFFE\uFFFF> without base',
    ],
  },
  'url-origin.any.js': {},
  'url-searchparams.any.js': {},
  'url-setters-a-area.window.js': {
    comment: 'Implement globalThis.document',
    skipAllTests: true,
  },
  'url-setters-stripping.any.js': {},
  'url-setters.any.js': {},
  'url-statics-canparse.any.js': {},
  'url-statics-parse.any.js': {},
  'url-tojson.any.js': {},
  'urlencoded-parser.any.js': {
    comment:
      'Requests fail due to HTTP method "LADIDA", responses fail due to shift_jis encoding',
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
  },
  'urlsearchparams-append.any.js': {},
  'urlsearchparams-constructor.any.js': {
    comment: 'Fix this eventually',
    expectedFailures: [
      'URLSearchParams constructor, DOMException as argument',
      'Construct with 2 unpaired surrogates (no trailing)',
      'Construct with 3 unpaired surrogates (no leading)',
      'Construct with object with NULL, non-ASCII, and surrogate keys',
    ],
  },
  'urlsearchparams-delete.any.js': {},
  'urlsearchparams-foreach.any.js': {},
  'urlsearchparams-get.any.js': {},
  'urlsearchparams-getall.any.js': {},
  'urlsearchparams-has.any.js': {},
  'urlsearchparams-set.any.js': {},
  'urlsearchparams-size.any.js': {},
  'urlsearchparams-sort.any.js': {},
  'urlsearchparams-stringifier.any.js': {},
} satisfies TestRunnerConfig;
