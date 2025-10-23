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

    // spans emitted by cache-test.js in execution order
    let expected = [
      {
        name: 'cache_match',
        'url.full': 'https://example.com/conditional-etag',
        closed: true,
      },
      {
        name: 'cache_match',
        'url.full': 'https://example.com/conditional-last-modified',
        closed: true,
      },
      {
        name: 'cache_delete',
        'url.full': 'https://example.com/delete-exists',
        closed: true,
      },
      {
        name: 'cache_delete',
        'url.full': 'https://example.com/delete-not-exists',
        closed: true,
      },
      {
        name: 'cache_delete',
        'url.full': 'https://example.com/delete-with-options',
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
        'url.full': 'https://example.com/cached-resource',
        closed: true,
      },
      {
        name: 'cache_match',
        'url.full': 'https://example.com/not-cached',
        closed: true,
      },
      {
        name: 'cache_match',
        'url.full': 'https://example.com/cached-with-options',
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
