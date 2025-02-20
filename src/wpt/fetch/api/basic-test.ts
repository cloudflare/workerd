// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'accept-header.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'conditional-get.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'error-after-response.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'header-value-combining.any.js': {
    comment: 'Might require a backend?',
    expectedFailures: [
    "response.headers.get('content-length') expects 0",
    "response.headers.get('content-length') expects 0, 0",
    "response.headers.get('double-trouble') expects , ",
    "response.headers.get('foo-test') expects 1, 2, 3",
    "response.headers.get('heya') expects , \x0B\f, 1, , , 2",
    "response.headers.get('www-authenticate') expects 1, 2, 3, 4"
    ]
  },
  'header-value-null-byte.any.js': {
    comment: 'Might require a backend?',
    expectedFailures: ['Ensure fetch() rejects null bytes in headers']
  },
  'historical.any.js': {
    comment: 'I guess we offer this method but we should not?',
    expectedFailures: [ 'Headers object no longer has a getAll() method' ]
  },
  'http-response-code.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'integrity.sub.any.js': {
    comment: 'Might need a backend, or maybe just letting it read top.txt',
    skipAllTests: true
  },
  'keepalive.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'mediasource.window.js': {
    comment: "We do not support MediaSource",
    skipAllTests: true,
  },
  'mode-no-cors.sub.any.js': {
    comment: 'Might need a backend, or maybe just letting it read top.txt',
    skipAllTests: true
  },
  'mode-same-origin.any.js': {
    comment: 'Might need a backend, or maybe just letting it read top.txt',
    skipAllTests: true
  },
  'referrer.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'request-forbidden-headers.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'request-head.any.js': {
    comment: 'Are we not blocking HEAD with body?',
    expectedFailures: [ 'Fetch with HEAD with body' ]
  },
  'request-headers.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'request-headers-case.any.js': {
    comment: 'Requires backend',
    expectedFailures: [
        'Multiple headers with the same name, different case (THIS-is-A-test first)',
  'Multiple headers with the same name, different case (THIS-IS-A-TEST first)'
    ]
  },
  'request-headers-nonascii.any.js': {
    comment: 'Investigate this',
    expectedFailures: [ 'Non-ascii bytes in request headers' ]
  },
  'request-private-network-headers.tentative.any.js': {
    comment: 'Does not appear to need a backend but still uses utils.js',
    skipAllTests: true
  },
  'request-referrer.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'request-upload.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'request-upload.h2.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'response-null-body.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'response-url.sub.any.js': {
    comment: 'Investigate this',
    expectedFailures: [
      'Testing response url getter with http://{{host}}:{{ports[http][0]}}/ada',
      'Testing response url getter with http://{{host}}:{{ports[http][0]}}/#',
      'Testing response url getter with http://{{host}}:{{ports[http][0]}}/#ada',
      'Testing response url getter with http://{{host}}:{{ports[http][0]}}#ada'
    ]
  },
  'scheme-about.any.js': {
    comment: 'Appears we do not follow the rules for the about scheme?',
    expectedFailures: [
      'Fetching about:blank with method GET is KO',
      'Fetching about:blank with method PUT is KO',
      'Fetching about:blank with method POST is KO',
      'Fetching about:invalid.com with method GET is KO',
      'Fetching about:config with method GET is KO',
      'Fetching about:unicorn with method GET is KO',
      'Fetching about:blank with range header does not affect behavior'
    ]
  },
  'scheme-blob.sub.any.js': {
    comment: 'URL.createObjectURL() is not implemented',
    skipAllTests: true
  },
  'scheme-data.any.js': {
    comment: 'We should maybe set response type to basic for data URLs? The last one seems to be weak validation of data urls?',
    expectedFailures: [
      'Fetching data:,response%27s%20body is OK',
      'Fetching data:,response%27s%20body is OK (same-origin)',
      'Fetching data:,response%27s%20body is OK (cors)',
      'Fetching data:text/plain;base64,cmVzcG9uc2UncyBib[...] is OK',
      'Fetching data:image/png;base64,cmVzcG9uc2UncyBib2[...] is OK',
      'Fetching [POST] data:,response%27s%20body is OK',
      'Fetching [HEAD] data:,response%27s%20body is OK',
      'Fetching [GET] data:notAdataUrl.com is KO'
    ]
  },
  'scheme-others.sub.any.js': {
    comment: 'Why is not rejecting these wacky schemes?',
    expectedFailures: [
      'Fetching aaa://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching cap://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching cid://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching dav://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching dict://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching dns://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching geo://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching im://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching imap://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching ipp://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching ldap://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching mailto://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching nfs://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching pop://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching rtsp://{{host}}:{{ports[http][0]}}/ is KO',
      'Fetching snmp://{{host}}:{{ports[http][0]}}/ is KO'
    ]
  },
  'status.h2.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'stream-response.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  },
  'stream-safe-creation.any.js': {
    before() {
      // @ts-expect-error Test expects some URL in location.href but doesn't actually care what it is.
      globalThis.location = {href: 'http://cloudflare.com'}
    },
  },
  'text-utf8.any.js': {
    comment: 'Requires backend',
    skipAllTests: true
  }

} satisfies TestRunnerConfig;
