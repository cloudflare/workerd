// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

export default {
  async fetch(request) {
    const cache = caches.default;

    // Test cache.match() - HIT case
    const matchRequest1 = new Request('https://example.com/cached-resource', {
      method: 'GET',
    });
    let matchResult = await cache.match(matchRequest1);

    // Test cache.match() - MISS case
    const matchRequest2 = new Request('https://example.com/not-cached', {
      method: 'GET',
    });
    matchResult = await cache.match(matchRequest2);

    // Test cache.match() with options
    const matchRequest3 = new Request('https://example.com/cached-with-options', {
      method: 'GET',
    });
    matchResult = await cache.match(matchRequest3, { ignoreMethod: false });

    // Test cache.put()
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

    // Test cache.put() with different cache control
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

    // Test cache.delete() - exists
    const deleteRequest1 = new Request('https://example.com/delete-exists', {
      method: 'GET',
    });
    let deleteResult = await cache.delete(deleteRequest1);

    // Test cache.delete() - doesn't exist
    const deleteRequest2 = new Request('https://example.com/delete-not-exists', {
      method: 'GET',
    });
    deleteResult = await cache.delete(deleteRequest2);

    // Test cache.delete() with options
    const deleteRequest3 = new Request('https://example.com/delete-with-options', {
      method: 'POST',
    });
    deleteResult = await cache.delete(deleteRequest3, { ignoreMethod: true });

    return new Response('Cache tests completed');
  },
};
