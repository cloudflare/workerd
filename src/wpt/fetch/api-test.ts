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
    comment: 'See individual tests',
    disabledTests: [
      // Flaky since 2025-06-09. To be investigated.
      'Stream errors once aborted, after reading. Underlying connection closed.',
      // Flaky since 2025-07-25. To be investigated.
      'Stream errors once aborted. Underlying connection closed.',
    ],
    expectedFailures: [
      // The fetch promise still resolves for some reason
      'Aborting rejects with AbortError',
      'Aborting rejects with abort reason',
      'Already aborted signal rejects immediately',

      // Doesn't reject
      'response.arrayBuffer() rejects if already aborted',
      'response.blob() rejects if already aborted',
      'response.bytes() rejects if already aborted',
      'response.json() rejects if already aborted',
      'response.text() rejects if already aborted',
      'Call text() twice on aborted response',

      // Instead throws TypeError: Parsing a Body as FormData requires a Content-Type header.
      'response.formData() rejects if already aborted',

      // ReadableStream.cancel was not called synchronously at the right time
      'Readable stream synchronously cancels with AbortError if aborted before reading',

      // Cloned request needs to have a different signal object, that's in the same state as the original
      'Signal state is cloned',
      'Signal on request object should also have abort reason',
      'Signal on request object',
    ],
  },
  'abort/request.any.js': {},

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
      "This test is too browser specific. It's assuming a request was served from cache vs received by server.",
    omittedTests: ['Testing conditional GET with ETags'],
  },
  'basic/error-after-response.any.js': {
    comment:
      'Stream disconnected prematurely and a dropped promise when faced with intentionally bad chunked encoding from WPT',
    disabledTests: true,
  },
  'basic/gc.any.js': {},
  'basic/header-value-combining.any.js': {
    comment:
      "Stream disconnected prematurely and a dropped promise. Not yet sure what is triggering about WPT's output",
    disabledTests: true,
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
    disabledTests: true,
  },
  'basic/keepalive.any.js': {
    comment: 'Keepalive is not implemented',
    disabledTests: true,
  },
  'basic/mediasource.window.js': {
    comment: 'MediaSource not implemented. It is DOM-specific',
    omittedTests: ['Cannot fetch blob: URL from a MediaSource'],
  },
  'basic/mode-no-cors.sub.any.js': {
    comment: 'Request.mode is not relevant to us',
    disabledTests: true,
  },
  'basic/mode-same-origin.any.js': {
    comment: 'Request.mode is not relevant to us',
    disabledTests: true,
  },
  'basic/referrer.any.js': {
    comment: 'Referrer is not implemented',
    omittedTests: true,
  },
  'basic/request-forbidden-headers.any.js': {
    comment: 'We do not have forbidden headers',
    omittedTests: true,
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
      // Float16Array not implemented
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
      // WPT is expecting us to insert some kind of User-Agent
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
    omittedTests: true,
  },
  'basic/request-referrer.any.js': {
    comment: 'Referrer is not implemented',
    omittedTests: true,
  },
  'basic/request-upload.any.js': {
    comment: 'Appears to corrupt the state of workerd',
    disabledTests: true,
  },
  'basic/request-upload.h2.any.js': {
    comment: 'Enable if HTTP/2 is implemented',
    disabledTests: true,
  },
  'basic/response-null-body.any.js': {
    comment:
      'Response begins with hello-worldHTTP/1.1 which will lead to invalid protocol errors coming up on other tests later on',
    disabledTests: true,
  },
  'basic/response-url.sub.any.js': {},
  'basic/scheme-about.any.js': {},
  'basic/scheme-blob.sub.any.js': {
    comment: 'URL.createObjectURL() is not implemented',
    disabledTests: true,
  },
  'basic/scheme-data.any.js': {
    comment: 'Response.type must be basic',
    expectedFailures: [
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
    comment: 'Enable if HTTP/2 is implemented',
    disabledTests: true,
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
      'We need to actually clone the body instead of taking a reference to it.',
    expectedFailures: ['TypedArray is cloned', 'ArrayBuffer is cloned'],
  },
  'body/formdata.any.js': {},
  'body/mime-type.any.js': {},

  'cors/cors-basic.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-cookies-redirect.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-cookies.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-expose-star.sub.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-filtering.sub.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-keepalive.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-multiple-origins.sub.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-no-preflight.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-origin.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-preflight-cache.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-preflight-not-cors-safelisted.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-preflight-redirect.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-preflight-referrer.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-preflight-response-validation.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-preflight-star.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-preflight-status.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-preflight.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-redirect-credentials.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-redirect-preflight.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'cors/cors-redirect.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
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
    comment:
      'Even though the actual bug was fixed (Authorization now stripped), these tests are failing due to certificate problems',
    expectedFailures: [
      'getAuthorizationHeaderValue - same origin redirection',
      'getAuthorizationHeaderValue - cross origin redirection',
    ],
  },
  'credentials/cookies.any.js': {
    comment: 'Request.credentials is not implemented',
    disabledTests: true,
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
    disabledTests: true,
  },
  'headers/headers-normalize.any.js': {},
  'headers/headers-record.any.js': {
    comment:
      'This test checks the exact order of operations in JS involved in accessing headers. Our implementation is in C++ instead.',
    omittedTests: true,
  },
  'headers/headers-structure.any.js': {},

  'idlharness.any.js': {
    comment: 'Implement IDL support in harness',
    disabledTests: true,
  },

  'policies/csp-blocked.js': {
    comment: 'CSP is not implemented',
    omittedTests: true,
  },
  'policies/nested-policy.js': {
    comment: 'CSP is not implemented',
    omittedTests: true,
  },
  'policies/referrer-no-referrer.js': {
    comment: 'CSP is not implemented',
    omittedTests: true,
  },
  'policies/referrer-origin-when-cross-origin.js': {
    comment: 'CSP is not implemented',
    omittedTests: true,
  },
  'policies/referrer-origin.js': {
    comment: 'CSP is not implemented',
    omittedTests: true,
  },
  'policies/referrer-unsafe-url.js': {
    comment: 'CSP is not implemented',
    omittedTests: true,
  },

  'redirect/redirect-back-to-original-origin.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'redirect/redirect-count.any.js': {},
  'redirect/redirect-empty-location.any.js': {
    comment:
      'Fix our handling of empty Location header. Even when fixed, tests will still fail due to CORS stuff',
    expectedFailures: [
      'redirect response with empty Location, manual mode',
      'redirect response with empty Location, follow mode',
    ],
  },
  'redirect/redirect-keepalive.any.js': {
    comment: 'Keepalive is not implemented',
    disabledTests: true,
  },
  'redirect/redirect-keepalive.https.any.js': {
    comment: 'Keepalive is not implemented',
    disabledTests: true,
  },
  'redirect/redirect-location-escape.tentative.any.js': {},
  'redirect/redirect-location.any.js': {
    comment:
      'Status is expected to be 0 in a browser to avoid leaking info. We do not implement CORS',
    omittedTests: [/Redirect 3\d\d in "manual" mode .*/],
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
    comment: "This test contains stuff besides CORS. Don't omit it all.",
    omittedTests: [
      /(same|cross)-origin redirect 3\d\d in manual redirect and (no-cors|cors) mode/,
      /cross-origin redirect 3\d\d in follow redirect and no-cors mode/,
      'manual redirect with a CORS error should be rejected',
    ],
  },
  'redirect/redirect-origin.any.js': {
    comment: 'CORS is not implemented',
    omittedTests: true,
  },
  'redirect/redirect-referrer-override.any.js': {
    comment: 'Referer is not implemented',
    omittedTests: true,
  },
  'redirect/redirect-referrer.any.js': {
    comment: 'Referrer is not implemented',
    omittedTests: true,
  },
  'redirect/redirect-schemes.any.js': {},
  'redirect/redirect-to-dataurl.any.js': {},
  'redirect/redirect-upload.h2.any.js': {
    comment: 'Do we support HTTP 2?',
    omittedTests: true,
  },

  'request/forbidden-method.any.js': {
    comment: 'We do not have forbidden methods',
    omittedTests: true,
  },
  'request/multi-globals/construct-in-detached-frame.window.js': {
    comment: "We don't support detached realms",
    omittedTests: true,
  },
  'request/request-bad-port.any.js': {},
  'request/request-cache-default-conditional.any.js': {
    comment: 'Will be enabled in a later PR',
    expectedFailures: true,
  },
  'request/request-cache-default.any.js': {
    comment: 'Will be enabled in a later PR',
    expectedFailures: true,
  },
  'request/request-cache-force-cache.any.js': {
    comment: 'Will be enabled in a later PR',
    expectedFailures: true,
  },
  'request/request-cache-no-cache.any.js': {
    comment: 'Will be enabled in a later PR',
    expectedFailures: true,
  },
  'request/request-cache-no-store.any.js': {
    comment: 'Will be enabled in a later PR',
    expectedFailures: true,
  },
  'request/request-cache-only-if-cached.any.js': {
    comment: 'Will be enabled in a later PR',
    expectedFailures: true,
  },
  'request/request-cache-reload.any.js': {
    comment: 'Will be enabled in a later PR',
    expectedFailures: true,
  },
  'request/request-cache.js': {
    comment: 'Will be enabled in a later PR',
    expectedFailures: true,
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
      'These tests would require us to throw errors for some invalid situations that we just ignore',
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
    omittedTests: true,
  },
  'request/request-init-002.any.js': {},
  'request/request-init-contenttype.any.js': {},
  'request/request-init-priority.any.js': {
    comment: 'Request.priority is not implemented',
    disabledTests: true,
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
    ],
  },
  'request/request-keepalive.any.js': {
    comment: 'keepalive is not implemented',
    disabledTests: true,
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
    omittedTests: true,
  },
  'response/response-blob-realm.any.js': {
    comment: 'Skipped because it involves iframes',
    omittedTests: true,
  },
  'response/response-cancel-stream.any.js': {},
  'response/response-clone-iframe.window.js': {
    comment: 'Skipped because it involves iframes',
    omittedTests: true,
  },
  'response/response-clone.any.js': {
    comment: 'TODO Investigate this',
    expectedFailures: [
      "Check Response's clone with default values, without body",
      'Check response clone use structureClone for teed ReadableStreams (Int8Arraychunk)',
      'Check response clone use structureClone for teed ReadableStreams (Int16Arraychunk)',
      'Check response clone use structureClone for teed ReadableStreams (Int32Arraychunk)',
      'Check response clone use structureClone for teed ReadableStreams (ArrayBufferchunk)',
      'Check response clone use structureClone for teed ReadableStreams (Uint8Arraychunk)',
      'Check response clone use structureClone for teed ReadableStreams (Uint8ClampedArraychunk)',
      'Check response clone use structureClone for teed ReadableStreams (Uint16Arraychunk)',
      'Check response clone use structureClone for teed ReadableStreams (Uint32Arraychunk)',
      'Check response clone use structureClone for teed ReadableStreams (BigInt64Arraychunk)',
      'Check response clone use structureClone for teed ReadableStreams (BigUint64Arraychunk)',
      'Check response clone use structureClone for teed ReadableStreams (Float16Arraychunk)',
      'Check response clone use structureClone for teed ReadableStreams (Float32Arraychunk)',
      'Check response clone use structureClone for teed ReadableStreams (Float64Arraychunk)',
      'Check response clone use structureClone for teed ReadableStreams (DataViewchunk)',
    ],
  },
  'response/response-consume-empty.any.js': {
    comment:
      'We seem to be returning the boundary value as text but WPT expects no value',
    expectedFailures: ['Consume empty FormData response body as text'],
  },
  'response/response-consume-stream.any.js': {},
  'response/response-error-from-stream.any.js': {
    comment: 'We have TypeError, they want pull Error',
    expectedFailures: [
      'ReadableStream start() Error propagates to Response.formData() Promise',
      'ReadableStream pull() Error propagates to Response.formData() Promise',
    ],
  },
  'response/response-error.any.js': {
    comment: 'Likely just missing validation',
    expectedFailures: ["Throws TypeError when responseInit's statusText is Ā"],
  },
  'response/response-from-stream.any.js': {
    comment: 'Missing expected exception (TypeError)',
    expectedFailures: [
      'Constructing a Response with a stream on which getReader() is called',
    ],
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
  'response/response-init-contenttype.any.js': {},
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
  'response/response-stream-bad-chunk.any.js': {},
  'response/response-stream-disturbed-1.any.js': {},
  'response/response-stream-disturbed-2.any.js': {},
  'response/response-stream-disturbed-3.any.js': {},
  'response/response-stream-disturbed-4.any.js': {},
  'response/response-stream-disturbed-5.any.js': {},
  'response/response-stream-disturbed-6.any.js': {},
  'response/response-stream-disturbed-by-pipe.any.js': {},
  'response/response-stream-disturbed-util.js': {},
  'response/response-stream-with-broken-then.any.js': {
    comment:
      'Triggers an internal error: promise.h:103: failed: expected Wrappable::tryUnwrapOpaque(isolate, handle) != nullptr',
    expectedFailures: [
      'Attempt to inject {done: false, value: bye} via Object.prototype.then.',
      'Attempt to inject value: undefined via Object.prototype.then.',
      'Attempt to inject undefined via Object.prototype.then.',
      'Attempt to inject 8.2 via Object.prototype.then.',
    ],
  },
} satisfies TestRunnerConfig;
