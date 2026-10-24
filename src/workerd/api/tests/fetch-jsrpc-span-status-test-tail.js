// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

import {
  invocationPromises,
  spans,
  testTailHandler,
} from 'test:instrumentation-tail';

export default testTailHandler;

// Expected status and exceptions of the span of a failed operation.
function failed(message, name = 'Error') {
  return {
    status: { code: 'error', message },
    exceptions: [{ name, message }],
  };
}

// Expected status of the span of a fetch that got an HTTP error status, which has no message.
const httpError = { status: { code: 'error' } };

export const test = {
  async test() {
    await Promise.allSettled(invocationPromises);
    const received = [...spans.values()];

    const fetchSpans = received.filter((span) => span.name === 'fetch');
    assert.deepStrictEqual(fetchSpans, expectedFetchSpans);

    // The caller's spans are in call order. Each call is its own invocation of the callee, so its
    // spans are sorted by method.
    const clientSpans = received.filter(
      (span) =>
        span.name === 'jsRpcCall' && span['jsrpc.target_kind'] === 'fetcher'
    );
    const serverSpans = received
      .filter(
        (span) =>
          span.name === 'jsRpcCall' &&
          span['jsrpc.target_kind'] === 'entrypoint'
      )
      .map(({ 'jsrpc.caller_span_id': _, ...span }) => span)
      .sort((a, b) => a['jsrpc.method'].localeCompare(b['jsrpc.method']));
    assert.deepStrictEqual(clientSpans, expectedClientSpans);
    assert.deepStrictEqual(serverSpans, expectedServerSpans);
  },
};

function fetchSpan(url, attributes) {
  return {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'GET',
    'url.full': url,
    ...attributes,
    closed: true,
  };
}

const expectedFetchSpans = [
  fetchSpan('http://backend/ok', {
    'http.response.status_code': 200n,
    'http.response.body.size': 2n,
  }),
  fetchSpan('http://backend/not-found', {
    'http.response.status_code': 404n,
    'http.response.body.size': 9n,
    ...httpError,
  }),
  fetchSpan('http://backend/error', {
    'http.response.status_code': 500n,
    'http.response.body.size': 5n,
    ...httpError,
  }),
  fetchSpan('http://unreachable/', failed('Network connection lost.')),
  fetchSpan('http://backend/slow', failed('fetch aborted')),
  // Both hops of a redirect have the URL of the request.
  fetchSpan('http://backend/redirect', {
    'http.response.status_code': 302n,
    'http.response.body.size': 0n,
  }),
  fetchSpan('http://backend/redirect', {
    'http.response.status_code': 302n,
    'http.response.body.size': 0n,
    ...failed(
      'Invalid Location header; unable to follow redirect.',
      'TypeError'
    ),
  }),
];

function rpcSpan(method, kind, attributes) {
  return {
    name: 'jsRpcCall',
    'jsrpc.method': method,
    'jsrpc.target_kind': kind,
    'jsrpc.operation': 'call',
    ...attributes,
    closed: true,
  };
}

const unserializable =
  'Could not serialize object of type "NotSerializable". This type does not support serialization.';

const expectedClientSpans = [
  rpcSpan('ok', 'fetcher'),
  rpcSpan('rejects', 'fetcher', failed('method failed')),
  rpcSpan(
    'throwsSync',
    'fetcher',
    failed('method failed synchronously', 'TypeError')
  ),
  rpcSpan(
    'returnsUnserializable',
    'fetcher',
    failed(unserializable, 'DataCloneError')
  ),
];

const expectedServerSpans = [
  rpcSpan('ok', 'entrypoint'),
  rpcSpan('rejects', 'entrypoint', failed('method failed')),
  rpcSpan(
    'returnsUnserializable',
    'entrypoint',
    failed(unserializable, 'DataCloneError')
  ),
  rpcSpan('run', 'entrypoint'),
  rpcSpan(
    'throwsSync',
    'entrypoint',
    failed('method failed synchronously', 'TypeError')
  ),
];
