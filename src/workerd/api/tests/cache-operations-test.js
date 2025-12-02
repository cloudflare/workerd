// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Cache API operations for generating instrumentation telemetry.
// The actual validation of telemetry spans happens in cache-instrumentation-test.js

import * as assert from 'node:assert';

// Note that these aren't really tests of the cache API. We run through a bunch
// of the cache API operations to emit telemetry for the tests in cache-instrumentation-test.js

export const matchOperations = {
  async test(ctrl, env, ctx) {
    const cache = caches.default;

    // Test cache.match() - HIT case
    const matchRequest1 = new Request('https://example.com/cached-resource', {
      method: 'GET',
    });
    let matchResult = await cache.match(matchRequest1);
    assert.ok(matchResult !== undefined, 'Should have a cache HIT');
    assert.strictEqual(await matchResult.text(), 'Cached content');

    // Test cache.match() - MISS case
    const matchRequest2 = new Request('https://example.com/not-cached', {
      method: 'GET',
    });
    matchResult = await cache.match(matchRequest2);
    assert.strictEqual(matchResult, undefined, 'Should be cache MISS');

    // Test cache.match() with options
    const matchRequest3 = new Request(
      'https://example.com/cached-with-options',
      {
        method: 'GET',
      }
    );
    matchResult = await cache.match(matchRequest3, { ignoreMethod: false });
    assert.strictEqual(
      matchResult,
      undefined,
      'Should be cache MISS with options'
    );
  },
};

export const putOperations = {
  async test(ctrl, env, ctx) {
    const cache = caches.default;

    // Test cache.put() with max-age
    const putRequest1 = new Request('https://example.com/put-resource', {
      method: 'GET',
    });
    const putResponse1 = new Response('Test content', {
      status: 200,
      headers: {
        'Content-Type': 'text/plain',
        'Cache-Control': 'public, max-age=3600',
      },
    });
    await cache.put(putRequest1, putResponse1);

    // Test cache.put() with no-store
    const putRequest2 = new Request('https://example.com/put-no-store', {
      method: 'GET',
    });
    const putResponse2 = new Response('No store content', {
      status: 200,
      headers: {
        'Content-Type': 'text/plain',
        'Cache-Control': 'no-store',
      },
    });
    await cache.put(putRequest2, putResponse2);

    // Test cache.put() with s-maxage
    const putRequest3 = new Request('https://example.com/put-s-maxage', {
      method: 'GET',
    });
    const putResponse3 = new Response('S-maxage content', {
      status: 200,
      headers: {
        'Content-Type': 'text/plain',
        'Cache-Control': 's-maxage=7200, public',
      },
    });
    await cache.put(putRequest3, putResponse3);
  },
};

export const deleteOperations = {
  async test(ctrl, env, ctx) {
    const cache = caches.default;

    // Test cache.delete() - exists
    const deleteRequest1 = new Request('https://example.com/delete-exists', {
      method: 'GET',
    });
    let deleteResult = await cache.delete(deleteRequest1);
    assert.strictEqual(
      deleteResult,
      true,
      'Should return true for successful delete'
    );

    // Test cache.delete() - doesn't exist
    const deleteRequest2 = new Request(
      'https://example.com/delete-not-exists',
      {
        method: 'GET',
      }
    );
    deleteResult = await cache.delete(deleteRequest2);
    assert.strictEqual(
      deleteResult,
      false,
      'Should return false when not found'
    );

    // Test cache.delete() with options
    const deleteRequest3 = new Request(
      'https://example.com/delete-with-options',
      {
        method: 'POST',
      }
    );
    deleteResult = await cache.delete(deleteRequest3, { ignoreMethod: true });
    assert.strictEqual(deleteResult, false, 'Should handle options');
  },
};

export const headersOperations = {
  async test(ctrl, env, ctx) {
    const cache = caches.default;

    // Test cache.put() with Cache-Tag header
    const putRequest1 = new Request('https://example.com/put-cache-tag', {
      method: 'GET',
    });
    const putResponse1 = new Response('Tagged content', {
      status: 200,
      headers: {
        'Content-Type': 'text/plain',
        'Cache-Control': 'public, max-age=3600',
        'Cache-Tag': 'tag1,tag2,tag3',
      },
    });
    await cache.put(putRequest1, putResponse1);

    // Test cache.put() with ETag header
    const putRequest2 = new Request('https://example.com/put-etag', {
      method: 'GET',
    });
    const putResponse2 = new Response('ETag content', {
      status: 200,
      headers: {
        'Content-Type': 'text/plain',
        'Cache-Control': 'public, max-age=3600',
        ETag: '"abc123"',
      },
    });
    await cache.put(putRequest2, putResponse2);

    // Test cache.put() with Expires header
    const putRequest3 = new Request('https://example.com/put-expires', {
      method: 'GET',
    });
    const putResponse3 = new Response('Expires content', {
      status: 200,
      headers: {
        'Content-Type': 'text/plain',
        Expires: 'Wed, 21 Oct 2025 07:28:00 GMT',
      },
    });
    await cache.put(putRequest3, putResponse3);

    // Test cache.put() with Last-Modified header
    const putRequest4 = new Request('https://example.com/put-last-modified', {
      method: 'GET',
    });
    const putResponse4 = new Response('Last-Modified content', {
      status: 200,
      headers: {
        'Content-Type': 'text/plain',
        'Cache-Control': 'public, max-age=3600',
        'Last-Modified': 'Wed, 21 Oct 2020 07:28:00 GMT',
      },
    });
    await cache.put(putRequest4, putResponse4);

    // Test cache.put() with Set-Cookie header (should not cache or require special handling)
    const putRequest5 = new Request('https://example.com/put-set-cookie', {
      method: 'GET',
    });
    const putResponse5 = new Response('Cookie content', {
      status: 200,
      headers: {
        'Content-Type': 'text/plain',
        'Cache-Control': 'public, max-age=3600',
        'Set-Cookie': 'sessionid=abc123; Path=/; HttpOnly',
      },
    });
    await cache.put(putRequest5, putResponse5);

    // Test cache.put() with Set-Cookie and Cache-Control: private=Set-Cookie
    const putRequest6 = new Request(
      'https://example.com/put-set-cookie-private',
      {
        method: 'GET',
      }
    );
    const putResponse6 = new Response('Cookie content with private', {
      status: 200,
      headers: {
        'Content-Type': 'text/plain',
        'Cache-Control': 'public, max-age=3600, private=Set-Cookie',
        'Set-Cookie': 'sessionid=xyz789; Path=/; HttpOnly',
      },
    });
    await cache.put(putRequest6, putResponse6);

    // Test cache.put() with must-revalidate
    const putRequest7 = new Request('https://example.com/put-must-revalidate', {
      method: 'GET',
    });
    const putResponse7 = new Response('Must revalidate content', {
      status: 200,
      headers: {
        'Content-Type': 'text/plain',
        'Cache-Control': 'public, max-age=3600, must-revalidate',
      },
    });
    await cache.put(putRequest7, putResponse7);

    // Test cache.put() with proxy-revalidate
    const putRequest8 = new Request(
      'https://example.com/put-proxy-revalidate',
      {
        method: 'GET',
      }
    );
    const putResponse8 = new Response('Proxy revalidate content', {
      status: 200,
      headers: {
        'Content-Type': 'text/plain',
        'Cache-Control': 'public, max-age=3600, proxy-revalidate',
      },
    });
    await cache.put(putRequest8, putResponse8);

    // Test cache.put() with private cache control
    const putRequest9 = new Request('https://example.com/put-private', {
      method: 'GET',
    });
    const putResponse9 = new Response('Private content', {
      status: 200,
      headers: {
        'Content-Type': 'text/plain',
        'Cache-Control': 'private, max-age=3600',
      },
    });
    await cache.put(putRequest9, putResponse9);

    // Test cache.put() with no-cache
    const putRequest10 = new Request('https://example.com/put-no-cache', {
      method: 'GET',
    });
    const putResponse10 = new Response('No-cache content', {
      status: 200,
      headers: {
        'Content-Type': 'text/plain',
        'Cache-Control': 'no-cache, max-age=3600',
      },
    });
    await cache.put(putRequest10, putResponse10);
  },
};

export const conditionalRequestOperations = {
  async test(ctrl, env, ctx) {
    const cache = caches.default;

    // Test cache.match() with If-None-Match (should work with ETag)
    const matchRequest1 = new Request('https://example.com/conditional-etag', {
      method: 'GET',
      headers: {
        'If-None-Match': 'abc123',
      },
    });
    let matchResult = await cache.match(matchRequest1);

    // Test cache.match() with If-Modified-Since (should work with Last-Modified)
    const matchRequest2 = new Request(
      'https://example.com/conditional-last-modified',
      {
        method: 'GET',
        headers: {
          'If-Modified-Since': 'Wed, 21 Oct 2020 07:28:00 GMT',
        },
      }
    );
    matchResult = await cache.match(matchRequest2);
  },
};
