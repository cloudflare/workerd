// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import util from 'node:util';

let scheduledLastCtrl;

export default {
  async fetch(request, env, ctx) {
    const { pathname } = new URL(request.url);
    if (pathname === '/body-length') {
      return Response.json(Object.fromEntries(request.headers));
    }
    if (pathname === '/web-socket') {
      const pair = new WebSocketPair();
      pair[0].addEventListener('message', (event) => {
        pair[0].send(util.inspect(event));
      });
      pair[0].accept();
      return new Response(null, {
        status: 101,
        webSocket: pair[1],
      });
    }
    return new Response(null, { status: 404 });
  },

  async scheduled(ctrl, env, ctx) {
    scheduledLastCtrl = ctrl;
    if (ctrl.cron === '* * * * 30') ctrl.noRetry();
  },

  async test(ctrl, env, ctx) {
    // Call `fetch()` with known body length
    {
      const body = new FixedLengthStream(3);
      const writer = body.writable.getWriter();
      void writer.write(new Uint8Array([1, 2, 3]));
      void writer.close();
      const response = await env.SERVICE.fetch(
        'http://placeholder/body-length',
        {
          method: 'POST',
          body: body.readable,
        }
      );
      const headers = new Headers(await response.json());
      assert.strictEqual(headers.get('Content-Length'), '3');
      assert.strictEqual(headers.get('Transfer-Encoding'), null);
    }

    // Check `fetch()` with unknown body length
    {
      const body = new IdentityTransformStream();
      const writer = body.writable.getWriter();
      void writer.write(new Uint8Array([1, 2, 3]));
      void writer.close();
      const response = await env.SERVICE.fetch(
        'http://placeholder/body-length',
        {
          method: 'POST',
          body: body.readable,
        }
      );
      const headers = new Headers(await response.json());
      assert.strictEqual(headers.get('Content-Length'), null);
      assert.strictEqual(headers.get('Transfer-Encoding'), 'chunked');
    }

    // Call `scheduled()` with no options
    {
      const result = await env.SERVICE.scheduled();
      assert.strictEqual(result.outcome, 'ok');
      assert(!result.noRetry);
      assert(Math.abs(Date.now() - scheduledLastCtrl.scheduledTime) < 3_000);
      assert.strictEqual(scheduledLastCtrl.cron, '');
    }

    // Call `scheduled()` with options, and noRetry()
    {
      const result = await env.SERVICE.scheduled({
        scheduledTime: 1000,
        cron: '* * * * 30',
      });
      assert.strictEqual(result.outcome, 'ok');
      assert(result.noRetry);
      assert.strictEqual(scheduledLastCtrl.scheduledTime, 1000);
      assert.strictEqual(scheduledLastCtrl.cron, '* * * * 30');
    }
  },
};

// inspect tests
export const test = {
  async test(ctrl, env, ctx) {
    // Check URL with duplicate search param keys
    const url = new URL('http://user:pass@placeholder:8787/path?a=1&a=2&b=3');
    assert.strictEqual(
      util.inspect(url),
      `URL {
  origin: 'http://placeholder:8787',
  href: 'http://user:pass@placeholder:8787/path?a=1&a=2&b=3',
  protocol: 'http:',
  username: 'user',
  password: 'pass',
  host: 'placeholder:8787',
  hostname: 'placeholder',
  port: '8787',
  pathname: '/path',
  search: '?a=1&a=2&b=3',
  hash: '',
  searchParams: URLSearchParams(3) { 'a' => '1', 'a' => '2', 'b' => '3' }
}`
    );

    // Check FormData with lower depth
    const formData = new FormData();
    formData.set('string', 'hello');
    formData.set(
      'blob',
      new Blob(['<h1>BLOB</h1>'], {
        type: 'text/html',
      })
    );
    formData.set(
      'file',
      new File(['password123'], 'passwords.txt', {
        type: 'text/plain',
        lastModified: 1000,
      })
    );
    assert.strictEqual(
      util.inspect(formData, { depth: 0 }),
      `FormData(3) { 'string' => 'hello', 'blob' => [File], 'file' => [File] }`
    );

    // Check request with mutable headers
    const request = new Request('http://placeholder', {
      method: 'POST',
      body: 'message',
      headers: { 'Content-Type': 'text/plain' },
    });
    if (env.CACHE_ENABLED) {
      assert.strictEqual(
        util.inspect(request),
        `Request {
  method: 'POST',
  url: 'http://placeholder',
  headers: Headers(1) { 'content-type' => 'text/plain', [immutable]: false },
  redirect: 'follow',
  fetcher: null,
  signal: AbortSignal { aborted: false, reason: undefined, onabort: null },
  cf: undefined,
  integrity: '',
  keepalive: false,
  cache: undefined,
  body: ReadableStream {
    locked: false,
    [state]: 'readable',
    [supportsBYOB]: true,
    [length]: 7n
  },
  bodyUsed: false
}`
      );
    } else {
      assert.strictEqual(
        util.inspect(request),
        `Request {
  method: 'POST',
  url: 'http://placeholder',
  headers: Headers(1) { 'content-type' => 'text/plain', [immutable]: false },
  redirect: 'follow',
  fetcher: null,
  signal: AbortSignal { aborted: false, reason: undefined, onabort: null },
  cf: undefined,
  integrity: '',
  keepalive: false,
  body: ReadableStream {
    locked: false,
    [state]: 'readable',
    [supportsBYOB]: true,
    [length]: 7n
  },
  bodyUsed: false
}`
      );
    }

    // Check response with immutable headers
    const response = await env.SERVICE.fetch('http://placeholder/not-found');
    assert.strictEqual(
      util.inspect(response),
      `Response {
  status: 404,
  statusText: 'Not Found',
  headers: Headers(0) { [immutable]: true },
  ok: false,
  redirected: false,
  url: 'http://placeholder/not-found',
  webSocket: null,
  cf: undefined,
  type: 'default',
  body: ReadableStream {
    locked: false,
    [state]: 'readable',
    [supportsBYOB]: true,
    [length]: 0n
  },
  bodyUsed: false
}`
    );

    // Check `MessageEvent` with unimplemented properties
    const webSocketResponse = await env.SERVICE.fetch(
      'http://placeholder/web-socket',
      {
        headers: { Upgrade: 'websocket' },
      }
    );
    const webSocket = webSocketResponse.webSocket;
    assert.notStrictEqual(webSocket, null);
    // The server-side WebSocketPair socket's binaryType depends on the compat flag.
    const bt = new WebSocketPair()[0].binaryType;
    const wsStr = `WebSocket {\n    readyState: 1,\n    url: null,\n    protocol: '',\n    extensions: '',\n    binaryType: '${bt}'\n  }`;
    const messagePromise = new Promise((resolve) => {
      webSocket.addEventListener('message', (event) => {
        assert.strictEqual(
          event.data,
          `MessageEvent {
  ports: [ [length]: 0 ],
  source: null,
  lastEventId: '',
  origin: null,
  data: 'data',
  type: 'message',
  eventPhase: 2,
  composed: false,
  bubbles: false,
  cancelable: false,
  defaultPrevented: false,
  returnValue: true,
  currentTarget: ${wsStr},
  target: ${wsStr},
  srcElement: ${wsStr},
  timeStamp: 0,
  isTrusted: true,
  cancelBubble: false,
  NONE: 0,
  CAPTURING_PHASE: 1,
  AT_TARGET: 2,
  BUBBLING_PHASE: 3
}`
        );
        resolve();
      });
    });
    webSocket.accept();
    webSocket.send('data');
    webSocket.close();
    await messagePromise;

    // Test sending to oversized URL (bigger than MAX_TRACE_BYTES), relevant primarily for tail worker test.
    await env.SERVICE.fetch('http://placeholder/' + '0'.repeat(2 ** 18));
  },
};

async function assertRequestCacheThrowsError(
  cacheHeader,
  errorName = 'Error',
  errorMessage = "The 'cache' field on 'RequestInitializerDict' is not implemented."
) {
  assert.throws(
    () => {
      new Request('https://example.org', { cache: cacheHeader });
    },
    {
      name: errorName,
      message: errorMessage,
    }
  );
}

async function assertFetchCacheRejectsError(
  cacheHeader,
  errorName = 'Error',
  errorMessage = "The 'cache' field on 'RequestInitializerDict' is not implemented."
) {
  await assert.rejects(
    (async () => {
      await fetch('https://example.org', { cache: cacheHeader });
    })(),
    {
      name: errorName,
      message: errorMessage,
    }
  );
}

export const cacheMode = {
  async test(ctrl, env, ctx) {
    var failureCases = [
      'default',
      'force-cache',
      'no-cache',
      'only-if-cached',
      'reload',
      'unsupported',
    ];
    assert.strictEqual('cache' in Request.prototype, env.CACHE_ENABLED);
    {
      const req = new Request('https://example.org', {});
      assert.strictEqual(req.cache, undefined);
    }
    if (!env.CACHE_ENABLED) {
      failureCases.push('no-store');
      for (const cacheMode in failureCases) {
        await assertRequestCacheThrowsError(cacheMode);
        await assertFetchCacheRejectsError(cacheMode);
      }
    } else {
      {
        const req = new Request('https://example.org', { cache: 'no-store' });
        assert.strictEqual(req.cache, 'no-store');
      }
      {
        const response = await env.SERVICE.fetch(
          'http://placeholder/not-found',
          { cache: 'no-store' }
        );
        assert.strictEqual(
          util.inspect(response),
          `Response {
  status: 404,
  statusText: 'Not Found',
  headers: Headers(0) { [immutable]: true },
  ok: false,
  redirected: false,
  url: 'http://placeholder/not-found',
  webSocket: null,
  cf: undefined,
  type: 'default',
  body: ReadableStream {
    locked: false,
    [state]: 'readable',
    [supportsBYOB]: true,
    [length]: 0n
  },
  bodyUsed: false
}`
        );
      }
      for (const cacheMode in failureCases) {
        await assertRequestCacheThrowsError(
          cacheMode,
          'TypeError',
          'Unsupported cache mode: ' + cacheMode
        );
        await assertFetchCacheRejectsError(
          cacheMode,
          'TypeError',
          'Unsupported cache mode: ' + cacheMode
        );
      }

      // Test cache mode compatibility with cf.cacheTtl
      // cache: 'no-store' with cf: { cacheTtl: -1 } should succeed because
      // -1 is the NOCACHE_TTL value that no-store sets automatically.
      {
        const req = new Request('https://example.org', {
          cache: 'no-store',
          cf: { cacheTtl: -1 },
        });
        assert.strictEqual(req.cache, 'no-store');
        // Fetch should succeed with compatible cacheTtl
        await env.SERVICE.fetch('http://placeholder/not-found', {
          cache: 'no-store',
          cf: { cacheTtl: -1 },
        });
      }

      // cache: 'no-store' with cf: { cacheTtl: 300 } should throw TypeError
      // because 300 is incompatible with no-store (validation happens at fetch time)
      {
        const req = new Request('https://example.org', {
          cache: 'no-store',
          cf: { cacheTtl: 300 },
        });
        // Request construction succeeds
        assert.strictEqual(req.cache, 'no-store');
        // But fetch should fail with incompatible cacheTtl
        await assert.rejects(
          env.SERVICE.fetch('http://placeholder/not-found', {
            cache: 'no-store',
            cf: { cacheTtl: 300 },
          }),
          {
            name: 'TypeError',
            message: /is not compatible with cache/,
          }
        );
      }

      // cache: 'no-store' with cf: { cacheTtl: 0 } should also throw TypeError
      {
        await assert.rejects(
          env.SERVICE.fetch('http://placeholder/not-found', {
            cache: 'no-store',
            cf: { cacheTtl: 0 },
          }),
          {
            name: 'TypeError',
            message: /is not compatible with cache/,
          }
        );
      }
    }
  },
};

// Tests for cf.cacheControl mutual exclusion and synthesis.
// These tests run regardless of cache option flag since cacheControl is always available.
export const cacheControlMutualExclusion = {
  async test(ctrl, env, ctx) {
    // cacheControl + cacheTtl → TypeError at construction time
    assert.throws(
      () =>
        new Request('https://example.org', {
          cf: { cacheControl: 'max-age=300', cacheTtl: 300 },
        }),
      {
        name: 'TypeError',
        message: /cacheControl.*cacheTtl.*mutually exclusive/,
      }
    );

    // cacheControl alone should succeed
    {
      const req = new Request('https://example.org', {
        cf: { cacheControl: 'public, max-age=3600' },
      });
      assert.ok(req.cf);
    }

    // cacheTtl alone should succeed
    {
      const req = new Request('https://example.org', {
        cf: { cacheTtl: 300 },
      });
      assert.ok(req.cf);
    }

    // cacheControl + cacheTtlByStatus should succeed (not mutually exclusive)
    {
      const req = new Request('https://example.org', {
        cf: {
          cacheControl: 'public, max-age=3600',
          cacheTtlByStatus: { '200-299': 86400 },
        },
      });
      assert.ok(req.cf);
    }

    // cacheControl with undefined cacheTtl should succeed (only non-undefined triggers conflict)
    {
      const req = new Request('https://example.org', {
        cf: { cacheControl: 'max-age=300', cacheTtl: undefined },
      });
      assert.ok(req.cf);
    }
  },
};

export const cacheControlWithCacheOption = {
  async test(ctrl, env, ctx) {
    if (!env.CACHE_ENABLED) return;

    // cache option + cf.cacheControl → TypeError at construction time
    assert.throws(
      () =>
        new Request('https://example.org', {
          cache: 'no-store',
          cf: { cacheControl: 'no-cache' },
        }),
      {
        name: 'TypeError',
        message: /cacheControl.*cannot be used together with the.*cache/,
      }
    );

    // cache: 'no-cache' + cf.cacheControl → also TypeError
    // (need cache_no_cache flag for this, skip if not available)
  },
};

export const cacheControlSynthesis = {
  async test(ctrl, env, ctx) {
    // When cacheTtl is set without cacheControl, cacheControl should be synthesized
    // in the serialized cf blob. We verify by checking the cf property roundtrips correctly.

    // cacheTtl: 300 → cacheControl should be synthesized as "max-age=300"
    {
      const req = new Request('https://example.org', {
        cf: { cacheTtl: 300 },
      });
      // The cf object at construction time won't have cacheControl yet —
      // synthesis happens at serialization (fetch) time in serializeCfBlobJson.
      // We can verify the request constructs fine.
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheTtl, 300);
    }

    // cacheTtl: -1 → cacheControl should be synthesized as "no-store"
    {
      const req = new Request('https://example.org', {
        cf: { cacheTtl: -1 },
      });
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheTtl, -1);
    }

    // cacheTtl: 0 → cacheControl should be synthesized as "max-age=0"
    {
      const req = new Request('https://example.org', {
        cf: { cacheTtl: 0 },
      });
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheTtl, 0);
    }

    // Explicit cacheControl should NOT be overwritten
    {
      const req = new Request('https://example.org', {
        cf: { cacheControl: 'public, s-maxage=86400' },
      });
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheControl, 'public, s-maxage=86400');
    }
  },
};

export const additionalCacheSettings = {
  async test(ctrl, env, ctx) {
    // All additional cache settings should be accepted on the cf object
    {
      const req = new Request('https://example.org', {
        cf: {
          cacheReserveEligible: true,
          respectStrongEtag: true,
          stripEtags: false,
          stripLastModified: false,
          cacheDeceptionArmor: true,
          cacheReserveMinimumFileSize: 1024,
        },
      });
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheReserveEligible, true);
      assert.strictEqual(req.cf.respectStrongEtag, true);
      assert.strictEqual(req.cf.stripEtags, false);
      assert.strictEqual(req.cf.stripLastModified, false);
      assert.strictEqual(req.cf.cacheDeceptionArmor, true);
      assert.strictEqual(req.cf.cacheReserveMinimumFileSize, 1024);
    }

    // Additional cache settings should work alongside cacheControl
    {
      const req = new Request('https://example.org', {
        cf: {
          cacheControl: 'public, max-age=3600',
          cacheReserveEligible: true,
          stripEtags: true,
        },
      });
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheControl, 'public, max-age=3600');
      assert.strictEqual(req.cf.cacheReserveEligible, true);
      assert.strictEqual(req.cf.stripEtags, true);
    }

    // Additional cache settings should work alongside cacheTtl
    {
      const req = new Request('https://example.org', {
        cf: {
          cacheTtl: 300,
          respectStrongEtag: true,
          cacheDeceptionArmor: true,
        },
      });
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheTtl, 300);
      assert.strictEqual(req.cf.respectStrongEtag, true);
      assert.strictEqual(req.cf.cacheDeceptionArmor, true);
    }
  },
};
