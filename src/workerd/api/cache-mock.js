// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Mock cache backend - responds to GET (match), PUT (put), and PURGE (delete)
export default {
  async fetch(request) {
    const url = new URL(request.url);
    const headers = new Headers();

    // Handle cache.match() operations (GET requests)
    if (request.method === 'GET') {
      // Check if this is a cache.match() request (has only-if-cached)
      const cacheControl = request.headers.get('cache-control');
      if (cacheControl?.includes('only-if-cached')) {
        // Simulate cache HIT
        if (url.pathname.includes('cached-resource')) {
          headers.set('CF-Cache-Status', 'HIT');
          return new Response('Cached content', { status: 200, headers });
        }
        // Simulate cache MISS
        headers.set('CF-Cache-Status', 'MISS');
        return new Response(null, { status: 504, headers });
      }
    }

    // Handle cache.put() operations (PUT requests)
    if (request.method === 'PUT') {
      // Read the body (which contains the serialized response to cache)
      await request.text();

      // Simulate successful cache write
      return new Response(null, { status: 204 });
    }

    // Handle cache.delete() operations (PURGE requests)
    if (request.method === 'PURGE') {
      // Simulate successful deletion
      if (url.pathname.includes('delete-exists')) {
        return new Response(null, { status: 200 });
      }
      // Simulate not found
      return new Response(null, { status: 404 });
    }

    return new Response('Not Found', { status: 404 });
  },
};
