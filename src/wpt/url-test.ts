// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'IdnaTestV2-removed.window.js': {},
  'IdnaTestV2.window.js': {
    comment:
      'WPT recently updated this test to check for Unicode 16 compliance',
    // We now have a huge number of failures
    // https://github.com/web-platform-tests/wpt/commit/d0cd7c05f70f6928234e90ddff90b39ef9c1eebc
    skipAllTests: true,
  },
  'historical.any.js': {},
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
    comment:
      'Replacer disables tests involving document.createElement. Expected failures are due to Unicode 15.1 and Unicode 16',
    expectedFailures: [
      // Unicode 15.1
      // Taken from https://github.com/nodejs/node/blob/5ab7c4c5b01e7579fd436000232f0f0484289d44/test/wpt/status/url.json#L13
      '\uD87E\uDC68.com (using URL)',
      '\uD87E\uDC68.com (using URL.host)',
      '\uD87E\uDC68.com (using URL.hostname)',
      // Unicode 16
      // https://github.com/web-platform-tests/wpt/commit/12de160159122dc4d0e3ee2d756444b249063e8a
      'look\u180Eout.net (using URL)',
      'look\u180Eout.net (using URL.host)',
      'look\u180Eout.net (using URL.hostname)',
      'look\u206Bout.net (using URL)',
      'look\u206Bout.net (using URL.host)',
      'look\u206Bout.net (using URL.hostname)',
      '\u04C0.com (using URL)',
      '\u04C0.com (using URL.host)',
      '\u04C0.com (using URL.hostname)',
      '\u2183.com (using URL)',
      '\u2183.com (using URL.host)',
      '\u2183.com (using URL.hostname)',
    ],
    replace: (code): string =>
      code.replace(/\["url", "a", "area"\]/, '[ "url" ]'),
  },
  'url-constructor.any.js': {
    comment: 'We are expected to encode ^ as %5E',
    expectedFailures: [
      'Parsing: <foo://host/ !"$%&\'()*+,-./:;<=>@[\\]^_`{|}~> without base',
      'Parsing: <wss://host/ !"$%&\'()*+,-./:;<=>@[\\]^_`{|}~> without base',
    ],
  },
  'url-origin.any.js': {},
  'url-searchparams.any.js': {},
  'url-setters-a-area.window.js': {
    comment: 'Skipped because it uses the same test data as url-setters.any.js',
    // Node does the same: https://github.com/nodejs/node/blob/5ab7c4c5b01e7579fd436000232f0f0484289d44/test/wpt/status/url.json#L24
    skipAllTests: true,
  },
  'url-setters-stripping.any.js': {},
  'url-setters.any.js': {
    comment: 'We are expected to encode ^ as %5E',
    expectedFailures: [
      "URL: Setting <a:/>.pathname = '\x00\x01\t\n" +
        "\r\x1F !\"#$%&'()*+,-./09:;<=>?@AZ[\\]^_`az{|}~\x7F\x80\x81Éé' UTF-8 percent encoding with the default encode set. Tabs and newlines are removed.",
    ],
  },
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
    expectedFailures: ['URLSearchParams constructor, DOMException as argument'],
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
