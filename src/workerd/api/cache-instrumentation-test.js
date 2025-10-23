// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';
import {
  invocationPromises,
  spans,
  testTailHandler,
} from 'test:instumentation-tail';

// Use shared instrumentation test tail worker
export default testTailHandler;

export const test = {
  async test() {
    // Wait for all the tailStream executions to finish
    await Promise.allSettled(invocationPromises);

    // Recorded streaming tail worker events, in insertion order,
    let received = Array.from(spans.values());

    // spans emitted by cache-operations-test.js in execution order
    let expected = [
      {
        name: 'cache_match',
        'cache.request.url': 'https://example.com/conditional-etag',
        'cache.request.method': 'GET',
        'cache.request.header.if_none_match': 'abc123',
        'cache.response.status_code': 504n,
        'cache.response.body.size': 0n,
        'cache.response.cache_status': 'MISS',
        'cache.response.success': false,
        closed: true,
      },
      {
        name: 'cache_match',
        'cache.request.url': 'https://example.com/conditional-last-modified',
        'cache.request.method': 'GET',
        'cache.request.header.if_modified_since':
          'Wed, 21 Oct 2020 07:28:00 GMT',
        'cache.response.status_code': 504n,
        'cache.response.body.size': 0n,
        'cache.response.cache_status': 'MISS',
        'cache.response.success': false,
        closed: true,
      },
      {
        name: 'cache_delete',
        'cache.request.url': 'https://example.com/delete-exists',
        'cache.request.method': 'GET',
        'cache.response.status_code': 200n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_delete',
        'cache.request.url': 'https://example.com/delete-not-exists',
        'cache.request.method': 'GET',
        'cache.response.status_code': 404n,
        'cache.response.success': false,
        closed: true,
      },
      {
        name: 'cache_delete',
        'cache.request.ignore_method': true,
        'cache.request.url': 'https://example.com/delete-with-options',
        'cache.request.method': 'POST',
        'cache.response.status_code': 404n,
        'cache.response.success': false,
        closed: true,
      },
      {
        name: 'cache_put',
        'cache.request.url': 'https://example.com/put-cache-tag',
        'cache.request.method': 'GET',
        'cache.request.payload.status_code': 200n,
        'cache.request.payload.header.cache_control': 'public, max-age=3600',
        'cache.request.payload.header.cache_tag': 'tag1,tag2,tag3',
        'cache.request.payload.size': 143n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_put',
        'cache.request.url': 'https://example.com/put-etag',
        'cache.request.method': 'GET',
        'cache.request.payload.status_code': 200n,
        'cache.request.payload.header.cache_control': 'public, max-age=3600',
        'cache.request.payload.header.etag': '"abc123"',
        'cache.request.payload.size': 130n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_put',
        'cache.request.url': 'https://example.com/put-expires',
        'cache.request.method': 'GET',
        'cache.request.payload.status_code': 200n,
        'cache.request.payload.header.expires': 'Wed, 21 Oct 2025 07:28:00 GMT',
        'cache.request.payload.size': 120n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_put',
        'cache.request.url': 'https://example.com/put-last-modified',
        'cache.request.method': 'GET',
        'cache.request.payload.status_code': 200n,
        'cache.request.payload.header.cache_control': 'public, max-age=3600',
        'cache.request.payload.header.last_modified':
          'Wed, 21 Oct 2020 07:28:00 GMT',
        'cache.request.payload.size': 169n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_put',
        'cache.request.url': 'https://example.com/put-set-cookie',
        'cache.request.method': 'GET',
        'cache.request.payload.status_code': 200n,
        'cache.request.payload.header.cache_control': 'public, max-age=3600',
        'cache.request.payload.size': 164n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_put',
        'cache.request.url': 'https://example.com/put-set-cookie-private',
        'cache.request.method': 'GET',
        'cache.request.payload.status_code': 200n,
        'cache.request.payload.header.cache_control':
          'public, max-age=3600, private=Set-Cookie',
        'cache.request.payload.size': 197n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_put',
        'cache.request.url': 'https://example.com/put-must-revalidate',
        'cache.request.method': 'GET',
        'cache.request.payload.status_code': 200n,
        'cache.request.payload.header.cache_control':
          'public, max-age=3600, must-revalidate',
        'cache.request.payload.size': 142n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_put',
        'cache.request.url': 'https://example.com/put-proxy-revalidate',
        'cache.request.method': 'GET',
        'cache.request.payload.status_code': 200n,
        'cache.request.payload.header.cache_control':
          'public, max-age=3600, proxy-revalidate',
        'cache.request.payload.size': 144n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_put',
        'cache.request.url': 'https://example.com/put-private',
        'cache.request.method': 'GET',
        'cache.request.payload.status_code': 200n,
        'cache.request.payload.header.cache_control': 'private, max-age=3600',
        'cache.request.payload.size': 118n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_put',
        'cache.request.url': 'https://example.com/put-no-cache',
        'cache.request.method': 'GET',
        'cache.request.payload.status_code': 200n,
        'cache.request.payload.header.cache_control': 'no-cache, max-age=3600',
        'cache.request.payload.size': 120n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_match',
        'cache.request.url': 'https://example.com/cached-resource',
        'cache.request.method': 'GET',
        'cache.response.status_code': 200n,
        'cache.response.body.size': 14n,
        'cache.response.cache_status': 'HIT',
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_match',
        'cache.request.url': 'https://example.com/not-cached',
        'cache.request.method': 'GET',
        'cache.response.status_code': 504n,
        'cache.response.body.size': 0n,
        'cache.response.cache_status': 'MISS',
        'cache.response.success': false,
        closed: true,
      },
      {
        name: 'cache_match',
        'cache.request.ignore_method': false,
        'cache.request.url': 'https://example.com/cached-with-options',
        'cache.request.method': 'GET',
        'cache.response.status_code': 504n,
        'cache.response.body.size': 0n,
        'cache.response.cache_status': 'MISS',
        'cache.response.success': false,
        closed: true,
      },
      {
        name: 'cache_put',
        'cache.request.url': 'https://example.com/put-resource',
        'cache.request.method': 'GET',
        'cache.request.payload.status_code': 200n,
        'cache.request.payload.header.cache_control': 'public, max-age=3600',
        'cache.request.payload.size': 114n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_put',
        'cache.request.url': 'https://example.com/put-no-store',
        'cache.request.method': 'GET',
        'cache.request.payload.status_code': 200n,
        'cache.request.payload.header.cache_control': 'no-store',
        'cache.request.payload.size': 106n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_put',
        'cache.request.url': 'https://example.com/put-s-maxage',
        'cache.request.method': 'GET',
        'cache.request.payload.status_code': 200n,
        'cache.request.payload.header.cache_control': 's-maxage=7200, public',
        'cache.request.payload.size': 119n,
        'cache.response.success': true,
        closed: true,
      },
    ];

    assert.equal(
      received.length,
      expected.length,
      `Expected ${expected.length} received ${received.length} spans`
    );
    let errors = [];
    for (let i = 0; i < received.length; i++) {
      try {
        assert.deepStrictEqual(received[i], expected[i]);
      } catch (e) {
        console.error(`value: ${i} does not match`);
        console.log(e);
        errors.push(e);
      }
    }
    if (errors.length > 0) {
      throw 'cache spans are incorrect';
    }
  },
};
