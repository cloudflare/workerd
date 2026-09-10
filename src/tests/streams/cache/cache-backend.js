// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Recording cache backend: a cache.put() arrives as a PUT whose body
// is the SERIALIZED HTTP RESPONSE (status line + headers + CRLFCRLF +
// body). The backend splits at the header boundary, verifies the
// continuous byte pattern over the BODY, extracts the serialized
// head's Content-Length, and records it all so the test worker can
// verify integrity via its MOCK service binding (/last-put). GETs
// serve the cache.match protocol (only-if-cached → HIT for
// /cached-resource, else 504 MISS).

const PATTERN_MODULUS = 251;
let lastPut = null;

function findHeaderBoundary(bytes) {
  for (let i = 0; i + 3 < bytes.byteLength; i++) {
    if (
      bytes[i] === 13 &&
      bytes[i + 1] === 10 &&
      bytes[i + 2] === 13 &&
      bytes[i + 3] === 10
    ) {
      return i + 4;
    }
  }
  return -1;
}

export default {
  async fetch(request) {
    const url = new URL(request.url);

    if (request.method === 'PUT') {
      const bytes = new Uint8Array(await request.arrayBuffer());
      const bodyStart = findHeaderBoundary(bytes);
      const head = new TextDecoder().decode(bytes.subarray(0, bodyStart));
      const declaredLength = /content-length:\s*(\d+)/i.exec(head)?.[1] ?? null;
      let patternOk = bodyStart >= 0;
      for (let i = bodyStart; i < bytes.byteLength; i++) {
        if (bytes[i] !== (i - bodyStart) % PATTERN_MODULUS) {
          patternOk = false;
          break;
        }
      }
      lastPut = {
        url: url.href,
        byteLength: bytes.byteLength - bodyStart,
        declaredLength,
        patternOk,
      };
      return new Response(null, { status: 204 });
    }

    if (request.method === 'GET') {
      if (url.pathname === '/last-put') {
        return Response.json(lastPut);
      }
      const cacheControl = request.headers.get('cache-control');
      if (cacheControl?.includes('only-if-cached')) {
        if (url.pathname.includes('cached-resource')) {
          return new Response('Cached content', {
            status: 200,
            headers: { 'CF-Cache-Status': 'HIT' },
          });
        }
        return new Response(null, {
          status: 504,
          headers: { 'CF-Cache-Status': 'MISS' },
        });
      }
    }

    return new Response('Not Found', { status: 404 });
  },
};
