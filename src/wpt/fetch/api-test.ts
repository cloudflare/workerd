// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'abort/cache.https.any.js': {
    comment: 'Not implemented',
    expectedFailures: [
      'Signals are not stored in the cache API',
      "Signals are not stored in the cache API, even if they're already aborted",
    ],
  },
  'abort/general.any.js': {
    comment: 'These tests will be enabled in a later PR',
    skipAllTests: true,
  },
  'abort/request.any.js': {
    comment: 'These tests will be enabled in a later PR',
    skipAllTests: true,
  },

  'basic/accept-header.any.js': {
    comment: 'Response.type must be basic',
    expectedFailures: [
      "Request through fetch should have 'accept' header with value '*/*'",
      "Request through fetch should have 'accept' header with value 'custom/*'",
      "Request through fetch should have 'accept-language' header with value 'bzh'",
      "Request through fetch should have a 'accept-language' header",
    ],
  },
  'basic/conditional-get.any.js': {
    comment:
      "It seems like they expect us to use the ETag if re-requested but return 200 OK anyway. Don't quite get it.",
    expectedFailures: ['Testing conditional GET with ETags'],
  },
  'basic/error-after-response.any.js': {
    comment:
      'Stream disconnected prematurely and a dropped promise when faced with intentionally bad chunked encoding from WPT',
    skipAllTests: true,
  },
  'basic/gc.any.js': {
    comment: 'Run WPT tests with --expose-gc if we want to run this test',
    skipAllTests: true,
  },
  'basic/header-value-combining.any.js': {
    comment:
      "Stream disconnected prematurely and a dropped promise. Not yet sure what is triggering about WPT's output",
    skipAllTests: true,
  },
  'basic/header-value-null-byte.any.js': {
    comment: 'We should return a nicer TypeError instead of "internal error"',
    expectedFailures: ['Ensure fetch() rejects null bytes in headers'],
  },
  'basic/historical.any.js': {
    comment: 'This test expects us not to implement getAll',
    expectedFailures: ['Headers object no longer has a getAll() method'],
  },
  'basic/http-response-code.any.js': {},
  'basic/integrity.sub.any.js': {
    comment: 'Integrity is not implemented',
    skipAllTests: true,
  },
  'basic/keepalive.any.js': {
    comment: 'Hard to run - involves iframes and workers',
    expectedFailures: [
      "[keepalive] simple GET request on 'load' [no payload]; setting up",
      "[keepalive] simple GET request on 'unload' [no payload]; setting up",
      "[keepalive] simple GET request on 'pagehide' [no payload]; setting up",
      "[keepalive] simple POST request on 'load' [no payload]; setting up",
      "[keepalive] simple POST request on 'unload' [no payload]; setting up",
      "[keepalive] simple POST request on 'pagehide' [no payload]; setting up",
      'simple keepalive test for web workers;',
    ],
  },
  'basic/mediasource.window.js': {
    comment: 'MediaSource not implemented',
    expectedFailures: ['Cannot fetch blob: URL from a MediaSource'],
  },
  'basic/mode-no-cors.sub.any.js': {
    comment: 'Request.mode is not relevant to us',
    skipAllTests: true,
  },
  'basic/mode-same-origin.any.js': {
    comment: 'Request.mode is not relevant to us',
    skipAllTests: true,
  },
  'basic/referrer.any.js': {
    comment: 'Referrer is not implemented',
    skipAllTests: true,
  },
  'basic/request-forbidden-headers.any.js': {
    comment: 'We do not have forbidden headers',
    skipAllTests: true,
  },
  'basic/request-head.any.js': {},
  'basic/request-headers-case.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'Multiple headers with the same name, different case (THIS-IS-A-TEST first)',
      'Multiple headers with the same name, different case (THIS-is-A-test first)',
    ],
  },
  'basic/request-headers-nonascii.any.js': {},
  'basic/request-headers.any.js': {
    comment: 'Reasons listed per test case',
    expectedFailures: [
      // Float16Array not implemengted
      'Fetch with POST with Float16Array body',
      // Fake HTTP method names not accepted
      'Fetch with Chicken',
      'Fetch with Chicken with body',
      'Fetch with TacO and mode "same-origin" needs an Origin header',
      'Fetch with TacO and mode "cors" needs an Origin header',
      // WPT is expecting us to insert some kind of User-Agent
      'Fetch with POST with Blob body with mime type',
      'Fetch with PUT without body',
      'Fetch with POST with URLSearchParams body',
      'Fetch with POST with FormData body',
      // WPT expects Origin header but we don't support CORS
      'Fetch with PUT and mode "same-origin" needs an Origin header',
      'Fetch with PUT with body',
      'Fetch with POST and mode "no-cors" needs an Origin header',
      // WPT is epxecting us to insert some kind of User-Agent
      'Fetch with GET',
      'Fetch with POST with Blob body',
      'Fetch with POST with Float64Array body',
      'Fetch with POST with text body',
      'Fetch with POST with ArrayBuffer body',
      'Fetch with POST with Float32Array body',
      'Fetch with POST with Uint8Array body',
      'Fetch with POST with Int8Array body',
      'Fetch with POST without body',
      'Fetch with HEAD',
      'Fetch with POST and mode "same-origin" needs an Origin header',
      'Fetch with POST with DataView body',
      // Response.type must be basic
      'Fetch with GET and mode "cors" does not need an Origin header',
    ],
  },
  'basic/request-private-network-headers.tentative.any.js': {
    comment: 'We do not have forbidden headers',
    skipAllTests: true,
  },
  'basic/request-referrer.any.js': {
    comment: 'Referrer is not implemented',
    skipAllTests: true,
  },
  'basic/request-upload.any.js': {
    comment: 'Multiple reasons for failure; see below',
    skipAllTests: true,
  },
  'basic/request-upload.h2.any.js': {
    comment: 'Do we support HTTP 2?',
    skipAllTests: true,
  },
  'basic/response-null-body.any.js': {
    comment:
      'Response begins with hello-worldHTTP/1.1 which will lead to invalid protocol errors coming up on other tests later on',
    skipAllTests: true,
  },
  'basic/response-url.sub.any.js': {},
  'basic/scheme-about.any.js': {},
  'basic/scheme-blob.sub.any.js': {
    comment: 'URL.createObjectURL() is not implemented',
    skipAllTests: true,
  },
  'basic/scheme-data.any.js': {
    comment: 'Response.type must be basic',
    expectedFailures: [
      // For this test: we should not return body when invoking HEAD on data url
      'Fetching [HEAD] data:,response%27s%20body is OK',
      'Fetching data:,response%27s%20body is OK',
      'Fetching data:,response%27s%20body is OK (same-origin)',
      'Fetching data:,response%27s%20body is OK (cors)',
      'Fetching data:text/plain;base64,cmVzcG9uc2UncyBib[...] is OK',
      'Fetching data:image/png;base64,cmVzcG9uc2UncyBib2[...] is OK',
      'Fetching [POST] data:,response%27s%20body is OK',
    ],
  },
  'basic/scheme-others.sub.any.js': {},
  'basic/status.h2.any.js': {
    comment: 'Do we support HTTP 2?',
    skipAllTests: true,
  },
  'basic/stream-response.any.js': {},
  'basic/stream-safe-creation.any.js': {},
  'basic/text-utf8.any.js': {
    comment: 'Some kind of unicode nitpickiness. Needs investigation',
    expectedFailures: [
      'UTF-8 with BOM with Request.text()',
      'UTF-8 with BOM with Response.text()',
      'UTF-8 with BOM with fetched data (UTF-8 charset)',
      'UTF-8 with BOM with fetched data (UTF-16 charset)',
    ],
  },

  'body/cloned-any.js': {
    comment:
      'At a glance it seems like we are just taking references to them instead of cloning?',
    expectedFailures: ['TypedArray is cloned', 'ArrayBuffer is cloned'],
  },
  'body/formdata.any.js': {},
  'body/mime-type.any.js': {
    comment: 'They expected text/html but we kept text/plain',
    expectedFailures: ['_Response: Extract a MIME type with clone'],
  },

  'cors/cors-basic.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-cookies-redirect.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-cookies.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-expose-star.sub.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-filtering.sub.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-keepalive.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-multiple-origins.sub.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-no-preflight.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-origin.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-preflight-cache.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-preflight-not-cors-safelisted.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-preflight-redirect.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-preflight-referrer.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-preflight-response-validation.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-preflight-star.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-preflight-status.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-preflight.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-redirect-credentials.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-redirect-preflight.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'cors/cors-redirect.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },

  'crashtests/huge-fetch.any.js': {},

  'credentials/authentication-basic.any.js': {
    comment: 'Response.type must be basic',
    expectedFailures: [
      'User-added Authorization header with include mode',
      'User-added Authorization header with omit mode',
      'User-added Authorization header with same-origin mode',
      'User-added bogus Authorization header with omit mode',
    ],
  },
  'credentials/authentication-redirection.any.js': {
    comment: 'I think this is ok because we do not care about CORS',
    expectedFailures: [
      'getAuthorizationHeaderValue - same origin redirection',
      'getAuthorizationHeaderValue - cross origin redirection',
    ],
  },
  'credentials/cookies.any.js': {
    comment: 'Request.credentials is not implemented',
    skipAllTests: true,
  },

  'headers/header-setcookie.any.js': {
    comment: '(1, 2) To be investigated, (3) CORS is not implemented',
    expectedFailures: [
      'Headers iterator is correctly updated with set-cookie changes',
      'Headers iterator is correctly updated with set-cookie changes #2',
      'Set-Cookie is a forbidden response header',
    ],
  },
  'headers/header-values-normalize.any.js': {
    comment: 'XMLHttpRequest is not supported',
    expectedFailures: [
      'XMLHttpRequest with value %00',
      'XMLHttpRequest with value %01',
      'XMLHttpRequest with value %02',
      'XMLHttpRequest with value %03',
      'XMLHttpRequest with value %04',
      'XMLHttpRequest with value %05',
      'XMLHttpRequest with value %06',
      'XMLHttpRequest with value %07',
      'XMLHttpRequest with value %08',
      'XMLHttpRequest with value %09',
      'XMLHttpRequest with value %0A',
      'XMLHttpRequest with value %0D',
      'XMLHttpRequest with value %0E',
      'XMLHttpRequest with value %0F',
      'XMLHttpRequest with value %10',
      'XMLHttpRequest with value %11',
      'XMLHttpRequest with value %12',
      'XMLHttpRequest with value %13',
      'XMLHttpRequest with value %14',
      'XMLHttpRequest with value %15',
      'XMLHttpRequest with value %16',
      'XMLHttpRequest with value %17',
      'XMLHttpRequest with value %18',
      'XMLHttpRequest with value %19',
      'XMLHttpRequest with value %1A',
      'XMLHttpRequest with value %1B',
      'XMLHttpRequest with value %1C',
      'XMLHttpRequest with value %1D',
      'XMLHttpRequest with value %1E',
      'XMLHttpRequest with value %1F',
      'XMLHttpRequest with value %20',
    ],
  },
  'headers/header-values.any.js': {
    comment: 'XMLHTTPRequest is not implemented',
    expectedFailures: [
      'XMLHttpRequest with value x%00x needs to throw',
      'XMLHttpRequest with value x%0Ax needs to throw',
      'XMLHttpRequest with value x%0Dx needs to throw',
      'XMLHttpRequest with all valid values',
    ],
  },
  'headers/headers-basic.any.js': {
    comment: 'Investigate our Headers implementation',
    expectedFailures: [
      'Create headers with existing headers with custom iterator',
      'Iteration skips elements removed while iterating',
      'Removing elements already iterated over causes an element to be skipped during iteration',
      'Appending a value pair during iteration causes it to be reached during iteration',
      'Prepending a value pair before the current element position causes it to be skipped during iteration and adds the current element a second time',
    ],
  },
  'headers/headers-casing.any.js': {},
  'headers/headers-combine.any.js': {},
  'headers/headers-errors.any.js': {
    comment: 'Our validation of header names is too lax',
    expectedFailures: [
      'Create headers giving bad header name as init argument',
      'Create headers giving bad header value as init argument',
      'Check headers get with an invalid name invalidĀ',
      'Check headers delete with an invalid name invalidĀ',
      'Check headers has with an invalid name invalidĀ',
      'Check headers set with an invalid name invalidĀ',
      'Check headers set with an invalid value invalidĀ',
      'Check headers append with an invalid name invalidĀ',
      'Check headers append with an invalid name [object Object]',
      'Check headers append with an invalid value invalidĀ',
    ],
  },
  'headers/headers-no-cors.any.js': {
    comment: 'Request.mode is not relevant',
    skipAllTests: true,
  },
  'headers/headers-normalize.any.js': {},
  'headers/headers-record.any.js': {
    comment: 'Investigate our Headers implementation',
    expectedFailures: [
      'Correct operation ordering with two properties',
      'Correct operation ordering with two properties one of which has an invalid name',
      'Correct operation ordering with two properties one of which has an invalid value',
      'Correct operation ordering with non-enumerable properties',
      'Correct operation ordering with undefined descriptors',
      'Basic operation with Symbol keys',
      'Operation with non-enumerable Symbol keys',
    ],
  },
  'headers/headers-structure.any.js': {},

  'idlharness.any.js': {
    comment: 'Does not contain any relevant tests',
    skipAllTests: true,
  },

  'policies/csp-blocked.js': {
    comment: 'CSP is not implemented',
    skipAllTests: true,
  },
  'policies/nested-policy.js': {
    comment: 'CSP is not implemented',
    skipAllTests: true,
  },
  'policies/referrer-no-referrer.js': {
    comment: 'CSP is not implemented',
    skipAllTests: true,
  },
  'policies/referrer-origin-when-cross-origin.js': {
    comment: 'CSP is not implemented',
    skipAllTests: true,
  },
  'policies/referrer-origin.js': {
    comment: 'CSP is not implemented',
    skipAllTests: true,
  },
  'policies/referrer-unsafe-url.js': {
    comment: 'CSP is not implemented',
    skipAllTests: true,
  },

  'redirect/redirect-back-to-original-origin.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'redirect/redirect-count.any.js': {},
  'redirect/redirect-empty-location.any.js': {
    comment:
      '(1) CORS is not implemented, our behaviour is OK, (2) We are expected to reject fetch in this case',
    expectedFailures: [
      'redirect response with empty Location, manual mode',
      'redirect response with empty Location, follow mode',
    ],
  },
  'redirect/redirect-keepalive.any.js': {
    comment: 'Keepalive is not implemented',
    expectedFailures: [
      '[keepalive][new window][unload] same-origin redirect; setting up',
      '[keepalive][new window][unload] same-origin redirect + preflight; setting up',
      '[keepalive][new window][unload] cross-origin redirect; setting up',
      '[keepalive][new window][unload] cross-origin redirect + preflight; setting up',
      '[keepalive][new window][unload] redirect to file URL; setting up',
      '[keepalive][new window][unload] redirect to data URL; setting up',
    ],
  },
  'redirect/redirect-keepalive.https.any.js': {
    comment: 'Keepalive is not implemented',
    expectedFailures: [
      '[keepalive][iframe][load] mixed content redirect; setting up',
    ],
  },
  'redirect/redirect-location-escape.tentative.any.js': {},
  'redirect/redirect-location.any.js': {
    comment: 'Manual mode apparently expects status to be 0 in these cases',
    expectedFailures: [
      'Redirect 301 in "manual" mode with invalid location',
      'Redirect 303 in "manual" mode without location',
      'Redirect 302 in "manual" mode with data location',
      'Redirect 308 in "manual" mode without location',
      'Redirect 302 in "manual" mode with valid location',
      'Redirect 302 in "manual" mode with invalid location',
      'Redirect 307 in "manual" mode with valid location',
      'Redirect 303 in "manual" mode with invalid location',
      'Redirect 307 in "manual" mode without location',
      'Redirect 308 in "manual" mode with data location',
      'Redirect 308 in "manual" mode with valid location',
      'Redirect 303 in "manual" mode with valid location',
      'Redirect 308 in "manual" mode with invalid location',
      'Redirect 307 in "manual" mode with invalid location',
      'Redirect 307 in "manual" mode with data location',
      'Redirect 303 in "manual" mode with data location',
      'Redirect 301 in "manual" mode with valid location',
      'Redirect 302 in "manual" mode without location',
      'Redirect 301 in "manual" mode without location',
      'Redirect 301 in "manual" mode with data location',
    ],
  },
  'redirect/redirect-method.any.js': {
    comment: 'Reasons listed per case',
    expectedFailures: [
      // Made up method name
      'Redirect 303 with TESTING',
      // Content-type header not cleared
      'Redirect 301 with POST',
      'Redirect 303 with POST',
      'Redirect 302 with POST',
      'Redirect 303 with HEAD',
      // Response.type must be basic
      'Redirect 302 with GET',
      'Redirect 307 with HEAD',
      'Redirect 301 with GET',
      'Redirect 303 with GET',
      'Redirect 307 with POST (string body)',
      'Redirect 307 with POST (blob body)',
      'Redirect 301 with HEAD',
      'Redirect 302 with HEAD',
      'Redirect 307 with GET',
    ],
  },
  'redirect/redirect-mode.any.js': {
    comment: 'CORS is not implemented',
    skipAllTests: true,
  },
  'redirect/redirect-origin.any.js': {
    comment: 'CORS is not implemented',
    expectedFailures: [
      '[POST] Redirect 302 Same origin to other origin',
      '[GET] Redirect 308 Other origin to same origin',
      '[POST] Redirect 307 Other origin to same origin',
      '[GET] Redirect 308 Other origin to other origin',
      '[GET] Redirect 302 Same origin to other origin',
      '[GET] Redirect 307 Other origin to other origin',
      '[POST] Redirect 308 Other origin to same origin',
      '[POST] Redirect 303 Same origin to other origin',
      '[GET] Redirect 301 Other origin to other origin',
      '[GET] Redirect 301 Same origin to other origin',
      '[GET] Redirect 302 Other origin to same origin',
      '[POST] Redirect 301 Other origin to other origin',
      '[GET] Redirect 303 Other origin to other origin',
      '[GET] Redirect 307 Other origin to same origin',
      '[POST] Redirect 302 Other origin to other origin',
      '[POST] Redirect 303 Other origin to other origin',
      '[POST] Redirect 303 Other origin to same origin',
      '[GET] Redirect 308 Same origin to other origin',
      '[GET] Redirect 302 Other origin to other origin',
      '[POST] Redirect 301 Same origin to other origin',
      '[POST] Redirect 302 Other origin to same origin',
      '[POST] Redirect 307 Same origin to other origin',
      '[GET] Redirect 301 Other origin to same origin',
      '[POST] Redirect 308 Other origin to other origin',
      '[POST] Redirect 308 Same origin to other origin',
      '[POST] Redirect 307 Other origin to other origin',
      '[GET] Redirect 307 Same origin to other origin',
      '[POST] Redirect 301 Other origin to same origin',
      '[GET] Redirect 303 Same origin to other origin',
      '[GET] Redirect 303 Other origin to same origin',
    ],
  },
  'redirect/redirect-referrer-override.any.js': {
    comment: 'Referer is not implemented',
    skipAllTests: true,
  },
  'redirect/redirect-referrer.any.js': {
    comment: 'Referrer is not implemented',
    skipAllTests: true,
  },
  'redirect/redirect-schemes.any.js': {},
  'redirect/redirect-to-dataurl.any.js': {},
  'redirect/redirect-upload.h2.any.js': {
    comment: 'Do we support HTTP 2?',
    skipAllTests: true,
  },

  'request/forbidden-method.any.js': {
    comment: 'We do not have forbidden methods',
    skipAllTests: true,
  },
  'request/multi-globals/construct-in-detached-frame.window.js': {
    comment: "We don't support detached realms",
    expectedFailures: [
      'creating a request from another request in a detached realm should work',
    ],
  },
  'request/request-bad-port.any.js': {},
  'request/request-cache-default-conditional.any.js': {
    comment: 'Will be enabled in a later PR',
    skipAllTests: true,
  },
  'request/request-cache-default.any.js': {
    comment: 'Will be enabled in a later PR',
    skipAllTests: true,
  },
  'request/request-cache-force-cache.any.js': {
    comment: 'Will be enabled in a later PR',
    skipAllTests: true,
  },
  'request/request-cache-no-cache.any.js': {
    comment: 'Will be enabled in a later PR',
    skipAllTests: true,
  },
  'request/request-cache-no-store.any.js': {
    comment: 'Will be enabled in a later PR',
    skipAllTests: true,
  },
  'request/request-cache-only-if-cached.any.js': {
    comment: 'Will be enabled in a later PR',
    skipAllTests: true,
  },
  'request/request-cache-reload.any.js': {
    comment: 'Will be enabled in a later PR',
    skipAllTests: true,
  },
  'request/request-cache.js': {
    comment: 'Will be enabled in a later PR',
    skipAllTests: true,
  },
  'request/request-constructor-init-body-override.any.js': {},
  'request/request-consume-empty.any.js': {
    comment:
      'We seem to be returning the boundary value as text but WPT expects no value',
    expectedFailures: ['Consume empty FormData request body as text'],
  },
  'request/request-consume.any.js': {},
  'request/request-disturbed.any.js': {
    comment:
      "(1) To be investigated, (2) They're literally passing URL, the constructor, as the URL!?!",
    expectedFailures: [
      'Input request used for creating new request became disturbed even if body is not used',
      'Request construction failure should not set "bodyUsed"',
    ],
  },
  'request/request-error.any.js': {
    comment:
      'These tests would require us to throw errors for some invalid situations that we just ingore',
    expectedFailures: [
      "RequestInit's window is not null",
      'Input URL has credentials',
      "RequestInit's mode is navigate",
      "RequestInit's referrer is invalid",
      "RequestInit's method is forbidden",
      "RequestInit's mode is no-cors and method is not simple",
      'Bad referrerPolicy init parameter value',
      'Bad mode init parameter value',
      'Bad credentials init parameter value',
      'Request with cache mode: only-if-cached and fetch mode: same-origin',
    ],
  },
  'request/request-error.js': {},
  'request/request-headers.any.js': {
    comment: 'Neither CORS nor header filtering is implemented',
    expectedFailures: [
      'Adding invalid request header "Accept-Charset: KO"',
      'Adding invalid request header "accept-charset: KO"',
      'Adding invalid request header "ACCEPT-ENCODING: KO"',
      'Adding invalid request header "Accept-Encoding: KO"',
      'Adding invalid request header "Access-Control-Request-Headers: KO"',
      'Adding invalid request header "Access-Control-Request-Method: KO"',
      'Adding invalid request header "Connection: KO"',
      'Adding invalid request header "Content-Length: KO"',
      'Adding invalid request header "Cookie: KO"',
      'Adding invalid request header "Cookie2: KO"',
      'Adding invalid request header "Date: KO"',
      'Adding invalid request header "DNT: KO"',
      'Adding invalid request header "Expect: KO"',
      'Adding invalid request header "Host: KO"',
      'Adding invalid request header "Keep-Alive: KO"',
      'Adding invalid request header "Origin: KO"',
      'Adding invalid request header "Referer: KO"',
      'Adding invalid request header "Set-Cookie: KO"',
      'Adding invalid request header "TE: KO"',
      'Adding invalid request header "Trailer: KO"',
      'Adding invalid request header "Transfer-Encoding: KO"',
      'Adding invalid request header "Upgrade: KO"',
      'Adding invalid request header "Via: KO"',
      'Adding invalid request header "Proxy-: KO"',
      'Adding invalid request header "proxy-a: KO"',
      'Adding invalid request header "Sec-: KO"',
      'Adding invalid request header "sec-b: KO"',
      'Adding invalid no-cors request header "Content-Type: KO"',
      'Adding invalid no-cors request header "Potato: KO"',
      'Adding invalid no-cors request header "proxy: KO"',
      'Adding invalid no-cors request header "proxya: KO"',
      'Adding invalid no-cors request header "sec: KO"',
      'Adding invalid no-cors request header "secb: KO"',
      'Adding invalid no-cors request header "Empty-Value: "',
      'Check that request constructor is filtering headers provided as init parameter',
      'Check that no-cors request constructor is filtering headers provided as init parameter',
      'Check that no-cors request constructor is filtering headers provided as part of request parameter',
    ],
  },
  'request/request-init-002.any.js': {},
  'request/request-init-contenttype.any.js': {
    comment:
      'We are expected to have a space between multipart/form-data and the boundary field',
    expectedFailures: ['Default Content-Type for Request with FormData body'],
  },
  'request/request-init-priority.any.js': {
    comment: 'Request.priority is not implemented',
    skipAllTests: true,
  },
  'request/request-init-stream.any.js': {
    comment: 'Most of these are because duplex is not implemented',
    expectedFailures: [
      'Constructing a Request with a stream holds the original object.',
      'It is error to omit .duplex when the body is a ReadableStream.',
      "It is error to set .duplex = 'full' when the body is null.",
      "It is error to set .duplex = 'full' when the body is a string.",
      "It is error to set .duplex = 'full' when the body is a Uint8Array.",
      "It is error to set .duplex = 'full' when the body is a Blob.",
      "It is error to set .duplex = 'full' when the body is a ReadableStream.",
      'Constructing a Request with a stream on which read() and releaseLock() are called',
    ],
  },
  'request/request-keepalive.any.js': {
    comment: 'keepalive is not implemented',
    skipAllTests: true,
  },
  'request/request-structure.any.js': {
    comment: 'Unimplemented or partially implemented fields',
    expectedFailures: [
      'Check destination attribute',
      'Check referrer attribute',
      'Check referrerPolicy attribute',
      'Check mode attribute',
      'Check credentials attribute',
      'Check cache attribute',
      'Check isReloadNavigation attribute',
      'Check isHistoryNavigation attribute',
      'Check duplex attribute',
    ],
  },

  'response/json.any.js': {
    comment: 'Investigate issues with our JSON parser',
    expectedFailures: [
      'Ensure UTF-16 results in an error',
      'Ensure the correct JSON parser is used',
    ],
  },
  'response/response-arraybuffer-realm.window.js': {
    comment: 'Skipped because it involves iframes',
    skipAllTests: true,
  },
  'response/response-blob-realm.any.js': {
    comment: 'Skipped because it involves iframes',
    skipAllTests: true,
  },
  'response/response-cancel-stream.any.js': {},
  'response/response-clone-iframe.window.js': {
    comment: 'Skipped because it involves iframes',
    skipAllTests: true,
  },
  'response/response-clone.any.js': {
    comment:
      'Several issues. Firstly, we require the type field to always be passed to ReadableStream',
    skipAllTests: true,
  },
  'response/response-consume-empty.any.js': {
    comment:
      'We seem to be returning the boundary value as text but WPT expects no value',
    expectedFailures: ['Consume empty FormData response body as text'],
  },
  'response/response-consume-stream.any.js': {
    comment:
      'Triggers UBSan error in BodyBufferInputStream::tryRead. memcpy from a NULL pointer. Yikes',
    skipAllTests: true,
  },
  'response/response-error-from-stream.any.js': {
    comment:
      'Several issues. Firstly, we require the type field to always be passed to ReadableStream',
    skipAllTests: true,
  },
  'response/response-error.any.js': {
    comment: 'Likely just missing validation',
    expectedFailures: ["Throws TypeError when responseInit's statusText is Ā"],
  },
  'response/response-from-stream.any.js': {
    comment:
      'Several issues. Firstly, we require the type field to always be passed to ReadableStream',
    skipAllTests: true,
  },
  'response/response-headers-guard.any.js': {
    comment: 'Likely just missing validation',
    expectedFailures: ['Ensure response headers are immutable'],
  },
  'response/response-init-001.any.js': {
    comment: 'statusText should be inited to OK',
    expectedFailures: ['Check default value for statusText attribute'],
  },
  'response/response-init-002.any.js': {},
  'response/response-init-contenttype.any.js': {
    comment:
      'We are inserting a space between multipart/form-data and the boundary',
    expectedFailures: ['Default Content-Type for Response with FormData body'],
  },
  'response/response-static-error.any.js': {
    comment:
      'We need to make Headers immutable when constructing Response.error()',
    expectedFailures: [
      "the 'guard' of the Headers instance should be immutable",
    ],
  },
  'response/response-static-json.any.js': {
    comment: 'statusText does not match the status code',
    expectedFailures: [
      // For this test specifically: failed to throw on non-encodable data
      'Check static json() throws when data is not encodable',
      'Check response returned by static json() with init undefined',
      'Check response returned by static json() with init {"status":400}',
      'Check response returned by static json() with init {"headers":{}}',
      'Check response returned by static json() with init {"headers":{"content-type":"foo/bar"}}',
      'Check response returned by static json() with init {"headers":{"x-foo":"bar"}}',
    ],
  },
  'response/response-static-redirect.any.js': {
    comment: 'statusText does not match the status code',
    expectedFailures: [
      'Check default redirect response',
      'Check response returned by static method redirect(), status = 301',
      'Check response returned by static method redirect(), status = 302',
      'Check response returned by static method redirect(), status = 303',
      'Check response returned by static method redirect(), status = 307',
      'Check response returned by static method redirect(), status = 308',
    ],
  },
  'response/response-stream-bad-chunk.any.js': {
    comment:
      'Several issues. Firstly, we require the type field to always be passed to ReadableStream',
    skipAllTests: true,
  },
  'response/response-stream-disturbed-1.any.js': {
    comment:
      'Several issues. Firstly, we require the type field to always be passed to ReadableStream',
    skipAllTests: true,
  },
  'response/response-stream-disturbed-2.any.js': {
    comment:
      'Several issues. Firstly, we require the type field to always be passed to ReadableStream',
    skipAllTests: true,
  },
  'response/response-stream-disturbed-3.any.js': {
    comment:
      'Several issues. Firstly, we require the type field to always be passed to ReadableStream',
    skipAllTests: true,
  },
  'response/response-stream-disturbed-4.any.js': {
    comment:
      'Several issues. Firstly, we require the type field to always be passed to ReadableStream',
    skipAllTests: true,
  },
  'response/response-stream-disturbed-5.any.js': {
    comment:
      'Several issues. Firstly, we require the type field to always be passed to ReadableStream',
    skipAllTests: true,
  },
  'response/response-stream-disturbed-6.any.js': {
    comment:
      'Several issues. Firstly, we require the type field to always be passed to ReadableStream',
    skipAllTests: true,
  },
  'response/response-stream-disturbed-by-pipe.any.js': {
    comment:
      'Several issues. Firstly, we require the type field to always be passed to ReadableStream',
    skipAllTests: true,
  },
  'response/response-stream-disturbed-util.js': {},
  'response/response-stream-with-broken-then.any.js': {
    comment:
      'Triggers an internal error: promise.h:103: failed: expected Wrappable::tryUnwrapOpaque(isolate, handle) != nullptr',
    expectedFailures: [
      'Attempt to inject {done: false, value: bye} via Object.prototype.then.',
      'Attempt to inject value: undefined via Object.prototype.then.',
      'Attempt to inject undefined via Object.prototype.then.',
      'Attempt to inject 8.2 via Object.prototype.then.',
      'intercepting arraybuffer to body readable stream conversion via Object.prototype.then should not be possible',
      'intercepting arraybuffer to text conversion via Object.prototype.then should not be possible',
    ],
  },
} satisfies TestRunnerConfig;
