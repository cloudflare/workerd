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
    // filtering spans not associated with Cache
    let received = Array.from(spans.values());
    console.log(received);

    // spans emitted by cache-operations.js in execution order
    let expected = [
      {
        name: 'cache_match',
        'cache.url': 'https://example.com/conditional-etag',
        'cache.header.if_none_match': 'abc123',
        'cache.response.status_code': 504n,
        'cache.response.body.size': 0n,
        'cache.response.cache_status': 'MISS',
        closed: true,
      },
      {
        name: 'cache_match',
        'cache.url': 'https://example.com/conditional-last-modified',
        'cache.header.if_modified_since': 'Wed, 21 Oct 2020 07:28:00 GMT',
        'cache.response.status_code': 504n,
        'cache.response.body.size': 0n,
        'cache.response.cache_status': 'MISS',
        closed: true,
      },
      {
        name: 'cache_delete',
        'cache.url': 'https://example.com/delete-exists',
        'cache.response.status_code': 200n,
        'cache.response.success': true,
        closed: true,
      },
      {
        name: 'cache_delete',
        'cache.url': 'https://example.com/delete-not-exists',
        'cache.response.status_code': 404n,
        'cache.response.success': false,
        closed: true,
      },
      {
        name: 'cache_delete',
        'cache.ignore_method': true,
        'cache.url': 'https://example.com/delete-with-options',
        'cache.response.status_code': 404n,
        'cache.response.success': false,
        closed: true,
      },
      {
        name: 'cache_put',
        'url.full': 'https://example.com/put-cache-tag',
        'cache_control.cacheability': 'public',
        'cache_control.expiration': 'max-age=3600',
        closed: true,
      },
      {
        name: 'cache_put',
        'url.full': 'https://example.com/put-etag',
        'cache_control.cacheability': 'public',
        'cache_control.expiration': 'max-age=3600',
        closed: true,
      },
      {
        name: 'cache_put',
        'url.full': 'https://example.com/put-expires',
        closed: true,
      },
      {
        name: 'cache_put',
        'url.full': 'https://example.com/put-last-modified',
        'cache_control.cacheability': 'public',
        'cache_control.expiration': 'max-age=3600',
        closed: true,
      },
      {
        name: 'cache_put',
        'url.full': 'https://example.com/put-set-cookie',
        'cache_control.cacheability': 'public',
        'cache_control.expiration': 'max-age=3600',
        closed: true,
      },
      {
        name: 'cache_put',
        'url.full': 'https://example.com/put-set-cookie-private',
        'cache_control.cacheability': 'private',
        closed: true,
      },
      {
        name: 'cache_put',
        'url.full': 'https://example.com/put-must-revalidate',
        'cache_control.cacheability': 'public',
        'cache_control.revalidation': 'must-revalidate',
        closed: true,
      },
      {
        name: 'cache_put',
        'url.full': 'https://example.com/put-proxy-revalidate',
        'cache_control.cacheability': 'public',
        'cache_control.revalidation': 'proxy-revalidate',
        closed: true,
      },
      {
        name: 'cache_put',
        'url.full': 'https://example.com/put-private',
        'cache_control.cacheability': 'private',
        'cache_control.expiration': 'max-age=3600',
        closed: true,
      },
      {
        name: 'cache_put',
        'url.full': 'https://example.com/put-no-cache',
        'cache_control.expiration': 'no-cache',
        closed: true,
      },
      {
        name: 'cache_match',
        'cache.url': 'https://example.com/cached-resource',
        'cache.response.status_code': 200n,
        'cache.response.body.size': 14n,
        'cache.response.cache_status': 'HIT',
        closed: true,
      },
      {
        name: 'cache_match',
        'cache.url': 'https://example.com/not-cached',
        'cache.response.status_code': 504n,
        'cache.response.body.size': 0n,
        'cache.response.cache_status': 'MISS',
        closed: true,
      },
      {
        name: 'cache_match',
        'cache.ignore_method': false,
        'cache.url': 'https://example.com/cached-with-options',
        'cache.response.status_code': 504n,
        'cache.response.body.size': 0n,
        'cache.response.cache_status': 'MISS',
        closed: true,
      },
      {
        name: 'cache_put',
        'url.full': 'https://example.com/put-resource',
        'cache_control.cacheability': 'public',
        'cache_control.expiration': 'max-age=3600',
        closed: true,
      },
      {
        name: 'cache_put',
        'url.full': 'https://example.com/put-no-store',
        'cache_control.cacheability': 'no-store',
        closed: true,
      },
      {
        name: 'cache_put',
        'url.full': 'https://example.com/put-s-maxage',
        'cache_control.cacheability': 'public',
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
