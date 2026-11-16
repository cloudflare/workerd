// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'compression-dictionary/resources/static/dictionary-hashes.js': {},
  'compression-dictionary/resources/static/script-001.js': {},
  'content-encoding/br/bad-br-body.https.any.js': {},
  'content-encoding/br/big-br-body.https.any.js': {},
  'content-encoding/br/br-body.https.any.js': {},
  'content-encoding/br/br-navigation.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'content-encoding/gzip/bad-gzip-body.any.js': {},
  'content-encoding/gzip/big-gzip-body.https.any.js': {},
  'content-encoding/gzip/gzip-body.any.js': {},
  'content-encoding/gzip/gzip-navigation.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'content-encoding/zstd/bad-zstd-body.https.any.js': {
    comment:
      'fetch() does not decode zstd Content-Encoding (system-streams.c++ handles gzip and br only)',
    expectedFailures: [
      'Consuming the body of a resource with bad zstd content with arrayBuffer() should reject',
      'Consuming the body of a resource with bad zstd content with blob() should reject',
      'Consuming the body of a resource with bad zstd content with bytes() should reject',
      'Consuming the body of a resource with bad zstd content with json() should reject',
      'Consuming the body of a resource with bad zstd content with text() should reject',
    ],
  },
  'content-encoding/zstd/big-zstd-body.https.any.js': {
    comment:
      'fetch() does not decode zstd Content-Encoding (system-streams.c++ handles gzip and br only)',
    expectedFailures: [
      'large zstd data should be decompressed successfully',
      'large zstd data should be decompressed successfully with byte stream',
    ],
  },
  'content-encoding/zstd/zstd-body.https.any.js': {
    comment:
      'fetch() does not decode zstd Content-Encoding (system-streams.c++ handles gzip and br only)',
    expectedFailures: [
      'fetched zstd data with content type text should be decompressed.',
      'fetched zstd data with content type octetstream should be decompressed.',
    ],
  },
  'content-encoding/zstd/zstd-navigation.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'content-length/api-and-duplicate-headers.any.js': {
    comment: 'Uses XMLHttpRequest',
    expectedFailures: [
      'XMLHttpRequest and duplicate Content-Length/Content-Type headers',
    ],
  },
  'content-length/parsing.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'content-length/too-long.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'content-type/multipart-malformed.any.js': {},
  'content-type/multipart.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'content-type/response.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'content-type/script.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'cross-origin-resource-policy/fetch.any.js': {
    comment:
      'Cross-Origin-Resource-Policy is a browser check on no-cors responses',
    omittedTests: true,
  },
  'cross-origin-resource-policy/fetch.https.any.js': {
    comment:
      'Cross-Origin-Resource-Policy is a browser check on no-cors responses',
    omittedTests: true,
  },
  'cross-origin-resource-policy/scheme-restriction.any.js': {
    comment:
      'Cross-Origin-Resource-Policy is a browser check on no-cors responses',
    omittedTests: true,
  },
  'cross-origin-resource-policy/scheme-restriction.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'cross-origin-resource-policy/syntax.any.js': {},
  'data-urls/base64.any.js': {},
  'data-urls/navigate.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'data-urls/processing.any.js': {},
  'fetch-later/activate-after.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/basic.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/basic.https.worker.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/iframe.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/new-window.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/non-secure.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/permissions-policy/deferred-fetch-allowed-by-permissions-policy-attribute-redirect.https.window.js':
    {
      comment: 'Needs a window or worker global',
      omittedTests: true,
    },
  'fetch-later/permissions-policy/deferred-fetch-allowed-by-permissions-policy-attribute.https.window.js':
    {
      comment: 'Needs a window or worker global',
      omittedTests: true,
    },
  'fetch-later/permissions-policy/deferred-fetch-allowed-by-permissions-policy.https.window.js':
    {
      comment: 'Needs a window or worker global',
      omittedTests: true,
    },
  'fetch-later/permissions-policy/deferred-fetch-default-permissions-policy.https.window.js':
    {
      comment: 'Needs a window or worker global',
      omittedTests: true,
    },
  'fetch-later/permissions-policy/deferred-fetch-supported-by-permissions-policy.window.js':
    {
      comment: 'Needs a window or worker global',
      omittedTests: true,
    },
  'fetch-later/policies/csp-allowed.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/policies/csp-blocked.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/policies/csp-redirect-to-blocked.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/accumulated-oversized-payload.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/cross-origin-iframe/accumulated-oversized-payload.https.window.js':
    {
      comment: 'Needs a window or worker global',
      omittedTests: true,
    },
  'fetch-later/quota/cross-origin-iframe/empty-payload.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/cross-origin-iframe/max-payload.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/cross-origin-iframe/multiple-iframes.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/cross-origin-iframe/oversized-payload.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/cross-origin-iframe/sandboxed-iframe.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/cross-origin-iframe/small-payload.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/empty-payload.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/max-payload.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/multiple-origins.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/oversized-payload.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/same-origin-iframe/accumulated-oversized-payload.https.window.js':
    {
      comment: 'Needs a window or worker global',
      omittedTests: true,
    },
  'fetch-later/quota/same-origin-iframe/empty-payload.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/same-origin-iframe/max-payload.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/same-origin-iframe/multiple-iframes.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/same-origin-iframe/oversized-payload.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/same-origin-iframe/sandboxed-iframe.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/same-origin-iframe/small-payload.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/quota/small-payload.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/send-on-deactivate-with-background-sync.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/send-on-deactivate.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/send-on-discard/not-send-after-abort.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'fetch-later/send-on-discard/send-multiple-with-activate-after.https.window.js':
    {
      comment: 'Needs a window or worker global',
      omittedTests: true,
    },
  'fetch-later/send-on-discard/send-multiple.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'h1-parsing/lone-cr.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'h1-parsing/resources-with-0x00-in-header.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'h1-parsing/status-code.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'http-cache/304-update.any.js': {
    comment: 'fetch() has no HTTP cache',
    expectedFailures: [
      'HTTP cache updates returned headers from a Last-Modified 304',
      'HTTP cache updates stored headers from a Last-Modified 304',
      'HTTP cache updates returned headers from a ETag 304',
      'HTTP cache updates stored headers from a ETag 304',
      'Content-* header',
    ],
  },
  'http-cache/cache-mode.any.js': {
    comment: 'fetch() has no HTTP cache',
    expectedFailures: [
      'Fetch sends Cache-Control: max-age=0 when cache mode is no-cache',
    ],
  },
  'http-cache/cc-request.any.js': {
    comment: 'fetch() has no HTTP cache',
    expectedFailures: [
      'HTTP cache does use aged stale response when request contains Cache-Control: max-stale that permits its use',
      'HTTP cache does reuse stale response with Age header when request contains Cache-Control: max-stale that permits its use',
      'HTTP cache generates 504 status code when nothing is in cache and request contains Cache-Control: only-if-cached',
    ],
  },
  'http-cache/freshness.any.js': {
    comment: 'fetch() has no HTTP cache',
    expectedFailures: [
      'HTTP cache reuses a response with a future Expires',
      'HTTP cache reuses a response with positive Cache-Control: max-age',
      'HTTP cache reuses a response with positive Cache-Control: max-age and a past Expires',
      'HTTP cache reuses a response with positive Cache-Control: max-age and an invalid Expires',
      'HTTP cache stores a response with Cache-Control: no-cache, but revalidates upon use',
      'HTTP cache stores a response with Cache-Control: no-cache, but revalidates upon use, even with max-age and Expires',
    ],
  },
  'http-cache/heuristic.any.js': {
    comment: 'fetch() has no HTTP cache',
    expectedFailures: [
      'HTTP cache reuses an unknown response with Last-Modified based upon heuristic freshness when Cache-Control: public is present',
      'HTTP cache reuses a 200 OK response with Last-Modified based upon heuristic freshness',
      'HTTP cache reuses a 203 Non-Authoritative Information response with Last-Modified based upon heuristic freshness',
      'HTTP cache reuses a 204 No Content response with Last-Modified based upon heuristic freshness',
      'HTTP cache reuses a 404 Not Found response with Last-Modified based upon heuristic freshness',
      'HTTP cache reuses a 405 Method Not Allowed response with Last-Modified based upon heuristic freshness',
      'HTTP cache reuses a 410 Gone response with Last-Modified based upon heuristic freshness',
      'HTTP cache reuses a 414 URI Too Long response with Last-Modified based upon heuristic freshness',
      'HTTP cache reuses a 501 Not Implemented response with Last-Modified based upon heuristic freshness',
    ],
  },
  'http-cache/http-cache.js': {},
  'http-cache/invalidate.any.js': {
    comment:
      'fetch() has no HTTP cache, and workerd rejects the custom request method (#3453)',
    expectedFailures: [
      'HTTP cache does not invalidate after a failed response from an unsafe request',
      'HTTP cache invalidates after a successful response from an unknown method',
      'HTTP cache does not invalidate Location URL after a failed response from an unsafe request',
      'HTTP cache invalidates Location URL after a successful response from an unknown method',
      'HTTP cache does not invalidate Content-Location URL after a failed response from an unsafe request',
      'HTTP cache invalidates Content-Location URL after a successful response from an unknown method',
    ],
  },
  'http-cache/partial.any.js': {
    comment: 'fetch() has no HTTP cache',
    expectedFailures: [
      'HTTP cache stores partial content and reuses it',
      'HTTP cache stores complete response and serves smaller ranges from it (byte-range-spec)',
      'HTTP cache stores complete response and serves smaller ranges from it (absent last-byte-pos)',
      'HTTP cache stores complete response and serves smaller ranges from it (suffix-byte-range-spec)',
      'HTTP cache stores complete response and serves smaller ranges from it with only-if-cached',
      'HTTP cache stores partial response and serves smaller ranges from it (byte-range-spec)',
      'HTTP cache stores partial response and serves smaller ranges from it (absent last-byte-pos)',
      'HTTP cache stores partial response and serves smaller ranges from it (suffix-byte-range-spec)',
      'HTTP cache stores partial content and completes it',
    ],
  },
  'http-cache/post-patch.any.js': {
    comment: 'fetch() has no HTTP cache',
    expectedFailures: [
      'HTTP cache uses content after PATCH request with response containing Content-Location and cache-allowing header',
      'HTTP cache uses content after POST request with response containing Content-Location and cache-allowing header',
    ],
  },
  'http-cache/status.any.js': {
    comment: 'fetch() has no HTTP cache',
    expectedFailures: [
      'HTTP cache avoids going to the network if it has a fresh 200 response',
      'HTTP cache avoids going to the network if it has a fresh 203 response',
      'HTTP cache avoids going to the network if it has a fresh 204 response',
      'HTTP cache avoids going to the network if it has a fresh 299 response',
      'HTTP cache avoids going to the network if it has a fresh 400 response',
      'HTTP cache avoids going to the network if it has a fresh 404 response',
      'HTTP cache avoids going to the network if it has a fresh 410 response',
      'HTTP cache avoids going to the network if it has a fresh 499 response',
      'HTTP cache avoids going to the network if it has a fresh 500 response',
      'HTTP cache avoids going to the network if it has a fresh 502 response',
      'HTTP cache avoids going to the network if it has a fresh 503 response',
      'HTTP cache avoids going to the network if it has a fresh 504 response',
      'HTTP cache avoids going to the network if it has a fresh 599 response',
    ],
  },
  'http-cache/vary.any.js': {
    comment: 'fetch() has no HTTP cache',
    expectedFailures: [
      'HTTP cache reuses Vary response when request matches',
      "HTTP cache doesn't invalidate existing Vary response",
      "HTTP cache doesn't pay attention to headers not listed in Vary",
      'HTTP cache reuses two-way Vary response when request matches',
      'HTTP cache reuses three-way Vary response when request matches',
      'HTTP cache uses three-way Vary response when both request and the original request omited a variant header',
    ],
  },
  'metadata/fetch-preflight.https.sub.any.js': {
    comment:
      'Sec-Fetch-* headers are browser-only, and /fetch/metadata/resources/helper.js is not bound',
    disabledTests: true,
  },
  'metadata/fetch.https.sub.any.js': {
    comment:
      'Sec-Fetch-* headers are browser-only, and /fetch/metadata/resources/helper.js is not bound',
    disabledTests: true,
  },
  'metadata/trailing-dot.https.sub.any.js': {
    comment:
      'Sec-Fetch-* headers are browser-only, and /fetch/metadata/resources/helper.js is not bound',
    disabledTests: true,
  },
  'nosniff/importscripts.js': {
    comment: 'Needs a worker global (importScripts, postMessage)',
    omittedTests: true,
  },
  'nosniff/parsing-nosniff.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'origin/assorted.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'range/blob.any.js': {
    comment: 'URL.createObjectURL() is not implemented',
    expectedFailures: [
      'A simple blob range request.',
      'A blob range request with no type.',
      'A blob range request with no end.',
      'A blob range request with no start.',
      'A simple blob range request with whitespace.',
      'Blob content with short content and a large range end',
      'Blob content with short content and a range end matching content length',
      'Blob range with whitespace before and after hyphen',
      'Blob range with whitespace after hyphen',
      'Blob range with whitespace around equals sign',
      'Blob range with no value',
      'Blob range with incorrect range header',
      'Blob range unit is case-sensitive',
      'Blob range unit is case-sensitive #2',
      'Blob range with incorrect range header #2',
      'Blob range with incorrect range header #3',
      'Blob range with U+000C where HTTP tab or space is allowed',
      'Blob range with U+000C before the dash',
      'Blob range with U+000B where HTTP tab or space is allowed',
      'Blob range request with multiple range values',
      'Blob range request with multiple range values and whitespace',
      'Blob range request with trailing comma',
      'Blob range with no start or end',
      'Blob range request with short range end',
      'Blob range start should be an ASCII digit',
      'Blob range should have a dash',
      'Blob range end should be an ASCII digit',
      'Blob range start must not be signed',
      'Blob range start must not be signed #2',
      'Blob range end must not be signed',
      "Blob range should include '-'",
      "Blob range should include '='",
      "Blob range should include 'bytes='",
      'Blob content with short content and a large range start',
      'Blob content with short content and a range start matching the content length',
    ],
  },
  'range/data.any.js': {
    comment: "Response.type is not 'basic' (#4709)",
    expectedFailures: [
      'data: URL and Range header',
      'data: URL and Range header with multiple ranges',
    ],
  },
  'range/general.any.js': {
    comment:
      'CORS and no-cors guards do not apply to workerd; Range requests are not sent with Accept-Encoding: identity',
    expectedFailures: [
      'Fetch with range header will be sent with Accept-Encoding: identity',
    ],
    omittedTests: [
      'Privileged header not allowed for guard type: request-no-cors',
      'Cross Origin Fetch with non safe range header',
      'Cross Origin Fetch with safe range header',
    ],
  },
  'range/general.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'range/sw-416.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'range/sw-intercept-range.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'range/sw-opaque.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'range/sw.https.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'redirects/data.window.js': {
    comment: 'Needs a window or worker global',
    omittedTests: true,
  },
  'security/1xx-response.any.js': {
    comment: 'A 1xx final response rejects with Error instead of TypeError',
    expectedFailures: [
      'Status(100) should be ignored.',
      'Status(103) should be ignored.',
      'Status(199) should be ignored.',
    ],
  },
  'security/dangling-markup/service-worker.js': {},
  'security/passthrough-serviceworker.js': {},
  'stale-while-revalidate/fetch.any.js': {
    comment: 'fetch() has no HTTP cache',
    expectedFailures: ['Second fetch returns same response'],
  },
  'stale-while-revalidate/sw-intercept.js': {},
} satisfies TestRunnerConfig;
