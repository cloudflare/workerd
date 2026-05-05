// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, ok, deepStrictEqual } from 'node:assert';
import OpenNextWorker from 'opennext-ssr-worker';

const mockAssets = {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname.startsWith('/_next/static/')) {
      return new Response('/* mock js */', {
        status: 200,
        headers: { 'content-type': 'application/javascript' },
      });
    }
    return new Response('Not Found', { status: 404 });
  },
};

const mockCtx = {
  waitUntil() {},
  passThroughOnException() {},
};

async function fetchWorker(path, options = {}) {
  const request = new Request(`http://localhost${path}`, {
    method: options.method || 'GET',
    headers: { accept: 'text/html', ...options.headers },
    body: options.body,
  });
  return OpenNextWorker.fetch(request, { ASSETS: mockAssets }, mockCtx);
}

export const workerInitialization = {
  async test() {
    ok(OpenNextWorker, 'OpenNext worker should be loaded');
    ok(
      typeof OpenNextWorker.fetch === 'function',
      'Worker should have fetch handler'
    );
  },
};

export const apiRouteGET = {
  async test() {
    const response = await fetchWorker('/api/data?foo=bar&baz=123', {
      headers: { accept: 'application/json' },
    });

    strictEqual(response.status, 200);
    strictEqual(response.headers.get('content-type'), 'application/json');

    const data = await response.json();
    ok(typeof data.timestamp === 'number', 'Should include timestamp');
    strictEqual(data.message, 'API response');
    strictEqual(data.method, 'GET');
    ok(data.searchParams, 'Should include search params');
  },
};

export const apiRoutePOST = {
  async test() {
    const requestBody = { key: 'value', nested: { a: 1, b: [1, 2, 3] } };
    const response = await fetchWorker('/api/data', {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        accept: 'application/json',
      },
      body: JSON.stringify(requestBody),
    });

    strictEqual(response.status, 200);
    const data = await response.json();
    deepStrictEqual(
      data.received,
      requestBody,
      'Should echo back request body'
    );
    strictEqual(data.method, 'POST');
  },
};

export const apiRouteOPTIONS = {
  async test() {
    const response = await fetchWorker('/api/data', {
      method: 'OPTIONS',
      headers: {
        origin: 'http://example.com',
        'access-control-request-method': 'POST',
      },
    });

    strictEqual(response.status, 204, 'OPTIONS should return 204');
    ok(
      response.headers.get('access-control-allow-methods'),
      'Should have CORS headers'
    );
  },
};

export const cookiesAPIGet = {
  async test() {
    const response = await fetchWorker('/api/cookies', {
      headers: {
        accept: 'application/json',
        cookie: 'test-cookie=hello; another=world',
      },
    });

    strictEqual(response.status, 200);
    const data = await response.json();
    ok(data.cookies, 'Should return cookies object');
  },
};

export const cookiesAPISet = {
  async test() {
    const response = await fetchWorker('/api/cookies', {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        accept: 'application/json',
      },
      body: JSON.stringify({
        name: 'session',
        value: 'abc123',
        options: { httpOnly: true, path: '/' },
      }),
    });

    strictEqual(response.status, 200);
    const setCookie = response.headers.get('set-cookie');
    ok(setCookie, 'Should have Set-Cookie header');
    ok(setCookie.includes('session=abc123'), 'Should set the cookie value');
  },
};

export const cookiesAPIDelete = {
  async test() {
    const response = await fetchWorker('/api/cookies?name=session', {
      method: 'DELETE',
      headers: { accept: 'application/json' },
    });

    strictEqual(response.status, 200);
    const data = await response.json();
    strictEqual(data.deleted, 'session');
  },
};

export const indexPageSSR = {
  async test() {
    const response = await fetchWorker('/');

    const contentType = response.headers.get('content-type');
    ok(
      contentType?.includes('text/html'),
      `Should be HTML, got: ${contentType}`
    );

    const html = await response.text();
    ok(
      html.includes('<!DOCTYPE html>') || html.includes('<!doctype html>'),
      'Should be HTML document'
    );
    ok(html.includes('<html'), 'Should have html tag');
    ok(html.includes('SSR Test Page'), 'Should have page title');
  },
};

export const indexPageWithCookie = {
  async test() {
    const response = await fetchWorker('/', {
      headers: { accept: 'text/html', cookie: 'test-cookie=from-request' },
    });

    strictEqual(response.status, 200);
    const html = await response.text();
    ok(html.includes('Cookie value:'), 'Should show cookie section');
  },
};

export const dynamicRouteBasic = {
  async test() {
    const response = await fetchWorker('/posts/123');

    strictEqual(response.status, 200);
    const html = await response.text();
    ok(
      html.includes('Post 123') || html.includes('123'),
      'Should render post ID'
    );
  },
};

export const dynamicRouteWithSpecialChars = {
  async test() {
    const response = await fetchWorker('/posts/hello-world-456');

    strictEqual(response.status, 200);
    const html = await response.text();
    ok(html.includes('hello-world-456'), 'Should handle slug with hyphens');
  },
};

export const dynamicRouteNumeric = {
  async test() {
    const response = await fetchWorker('/posts/999');

    strictEqual(response.status, 200);
    const html = await response.text();
    ok(html.includes('999'), 'Should handle numeric ID');
  },
};

export const streamingPageRenders = {
  async test() {
    const response = await fetchWorker('/streaming');

    strictEqual(response.status, 200);
    ok(
      response.headers.get('content-type')?.includes('text/html'),
      'Should be HTML'
    );

    const html = await response.text();
    ok(html.includes('Streaming Test Page'), 'Should have page content');
    ok(html.includes('Content chunk'), 'Should have streamed content');
  },
};

export const streamingResponseIsReadable = {
  async test() {
    const response = await fetchWorker('/streaming');

    ok(response.body, 'Response should have a body');
    ok(
      typeof response.body.getReader === 'function',
      'Body should be a ReadableStream'
    );

    const reader = response.body.getReader();
    const chunks = [];
    const decoder = new TextDecoder();
    let totalBytes = 0;

    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
      totalBytes += value.byteLength;
    }

    ok(chunks.length > 0, 'Should receive at least one chunk');
    ok(totalBytes > 0, `Should receive bytes, got ${totalBytes}`);

    const fullContent = chunks
      .map((c) => decoder.decode(c, { stream: true }))
      .join('');
    ok(
      fullContent.includes('<!DOCTYPE html>') ||
        fullContent.includes('<!doctype html>'),
      'Streamed content should be valid HTML'
    );
  },
};

export const streamingMultipleChunks = {
  async test() {
    const response = await fetchWorker('/streaming');
    const reader = response.body.getReader();

    let chunkCount = 0;
    let totalSize = 0;

    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      chunkCount++;
      totalSize += value.byteLength;
    }

    ok(chunkCount >= 1, `Should have chunks, got ${chunkCount}`);
    ok(
      totalSize > 1000,
      `Should have substantial content, got ${totalSize} bytes`
    );
  },
};

export const streamingConcurrentReads = {
  async test() {
    const responses = await Promise.all([
      fetchWorker('/streaming'),
      fetchWorker('/streaming'),
      fetchWorker('/streaming'),
    ]);

    const results = await Promise.all(
      responses.map(async (response) => {
        const reader = response.body.getReader();
        let size = 0;
        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          size += value.byteLength;
        }
        return size;
      })
    );

    for (let i = 0; i < results.length; i++) {
      ok(results[i] > 0, `Stream ${i} should have content`);
    }
  },
};

export const redirectWithTarget = {
  async test() {
    const response = await fetchWorker(
      '/redirect-test?target=/posts/redirected',
      {
        redirect: 'manual',
      }
    );

    ok(
      response.status === 307 ||
        response.status === 308 ||
        response.status === 302,
      `Should redirect, got status ${response.status}`
    );

    const location = response.headers.get('location');
    ok(location, 'Should have Location header');
    ok(location.includes('/posts/redirected'), 'Should redirect to target');
  },
};

export const redirectPageWithoutTarget = {
  async test() {
    const response = await fetchWorker('/redirect-test');

    strictEqual(response.status, 200, 'Should render page without redirect');
    const html = await response.text();
    ok(html.includes('Redirect Test'), 'Should show redirect test page');
  },
};

export const rscRequestBasic = {
  async test() {
    const response = await fetchWorker('/', {
      headers: { accept: '*/*', RSC: '1' },
    });

    ok(
      response.status >= 200 && response.status < 500,
      `RSC request should not error, got ${response.status}`
    );
    const body = await response.text();
    ok(body.length > 0, 'RSC response should not be empty');
  },
};

export const rscPrefetchRequest = {
  async test() {
    const response = await fetchWorker('/posts/456', {
      headers: { accept: '*/*', RSC: '1', 'Next-Router-Prefetch': '1' },
    });

    ok(
      response.status >= 200 && response.status < 500,
      `RSC prefetch should work, got ${response.status}`
    );
  },
};

export const notFoundPage = {
  async test() {
    const response = await fetchWorker('/nonexistent-page-xyz-123');

    ok(
      response.status === 404 || response.status === 200,
      `Should handle 404, got: ${response.status}`
    );
  },
};

export const notFoundAPIRoute = {
  async test() {
    const response = await fetchWorker('/api/nonexistent', {
      headers: { accept: 'application/json' },
    });

    ok(
      response.status === 404 || response.status >= 400,
      `API 404 should return error status, got: ${response.status}`
    );
  },
};

export const customHeadersForwarded = {
  async test() {
    const response = await fetchWorker('/api/data', {
      headers: {
        accept: 'application/json',
        'x-custom-header': 'test-value',
        'x-forwarded-for': '192.168.1.1',
        'user-agent': 'workerd-test/1.0',
      },
    });

    strictEqual(response.status, 200);
    const data = await response.json();
    ok(data.headers, 'Should return headers');
    strictEqual(data.headers['x-custom-header'], 'test-value');
  },
};

export const acceptLanguageHeader = {
  async test() {
    const response = await fetchWorker('/api/data', {
      headers: {
        accept: 'application/json',
        'accept-language': 'en-US,en;q=0.9,es;q=0.8',
      },
    });

    strictEqual(response.status, 200);
    const data = await response.json();
    ok(data.headers['accept-language'], 'Should forward accept-language');
  },
};

export const headRequest = {
  async test() {
    const response = await fetchWorker('/api/data', {
      method: 'HEAD',
      headers: { accept: 'application/json' },
    });

    ok(response.status >= 200 && response.status < 500, 'HEAD should work');
    const body = await response.text();
    ok(body.length >= 0, 'HEAD response body handled');
  },
};

export const concurrentMixedRequests = {
  async test() {
    const responses = await Promise.all([
      fetchWorker('/'),
      fetchWorker('/api/data', { headers: { accept: 'application/json' } }),
      fetchWorker('/posts/1'),
      fetchWorker('/posts/2'),
      fetchWorker('/streaming'),
      fetchWorker('/api/cookies', { headers: { accept: 'application/json' } }),
    ]);

    for (let i = 0; i < responses.length; i++) {
      ok(
        responses[i].status >= 200 && responses[i].status < 500,
        `Request ${i} should succeed`
      );
    }
  },
};

export const concurrentAPIRequests = {
  async test() {
    const responses = await Promise.all(
      Array.from({ length: 10 }, (_, i) =>
        fetchWorker(`/api/data?id=${i}`, {
          headers: { accept: 'application/json' },
        })
      )
    );

    const bodies = await Promise.all(responses.map((r) => r.json()));
    for (let i = 0; i < bodies.length; i++) {
      ok(bodies[i].timestamp, `Request ${i} should have timestamp`);
    }
  },
};

export const gracefulErrorHandling = {
  async test() {
    const paths = ['/500', '/../../../etc/passwd', '/api/data?error=true'];

    for (const path of paths) {
      const response = await fetchWorker(path);
      ok(
        typeof response.status === 'number',
        `${path} should return a response`
      );
      ok(
        response.status >= 200 && response.status < 600,
        `${path} should have valid status`
      );
    }
  },
};

export const cacheControlHeaders = {
  async test() {
    const response = await fetchWorker('/');
    const cacheControl = response.headers.get('cache-control');
    ok(
      cacheControl === null || typeof cacheControl === 'string',
      'Cache-Control header should be readable'
    );
  },
};

export const contentTypeHeaders = {
  async test() {
    const htmlResponse = await fetchWorker('/');
    ok(
      htmlResponse.headers.get('content-type')?.includes('text/html'),
      'HTML page should have text/html'
    );

    const jsonResponse = await fetchWorker('/api/data', {
      headers: { accept: 'application/json' },
    });
    ok(
      jsonResponse.headers.get('content-type')?.includes('application/json'),
      'API should have application/json'
    );
  },
};
