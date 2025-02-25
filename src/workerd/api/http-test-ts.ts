// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import util from 'node:util';

export default {
  async fetch(request: any, env: any, ctx: any) {
    const { pathname } = new URL(request.url);
    return new Response(null, { status: 404 });
  },
};

async function assertRequestCacheThrowsError(
  cacheHeader: RequestCache,
  errorName: String = 'Error',
  errorMessage: String = "The 'cache' field on 'RequestInitializerDict' is not implemented."
) {
  assert.throws(
    () => {
      const header = { cache: cacheHeader };
      const req: RequestInit = header;
      new Request('https://example.org', req);
    },
    {
      name: errorName,
      message: errorMessage,
    }
  );
}

async function assertFetchCacheRejectsError(
  cacheHeader: RequestCache,
  errorName: String = 'Error',
  errorMessage: String = "The 'cache' field on 'RequestInitializerDict' is not implemented."
) {
  await assert.rejects(
    (async () => {
      const header = { cache: cacheHeader };
      const req: RequestInit = header;
      await fetch('https://example.org', req);
    })(),
    {
      name: errorName,
      message: errorMessage,
    }
  );
}

export const cacheMode = {
  async test(ctrl: any, env: any, ctx: any) {
    const allowedCacheModes: Set<RequestCache> = new Set([
      'default',
      'force-cache',
      'no-cache',
      'no-store',
      'only-if-cached',
      'reload',
    ]);
    assert.strictEqual('cache' in Request.prototype, env.CACHE_ENABLED);
    {
      const req = new Request('https://example.org', {});
      assert.strictEqual(req.cache, undefined);
    }

    var enabledCacheModes: Set<RequestCache> = new Set();

    if (env.CACHE_ENABLED) {
      enabledCacheModes.add('no-store');
    }
    if (env.NO_CACHE_ENABLED) {
      enabledCacheModes.add('no-cache');
    }

    const failureCacheModes = allowedCacheModes.difference(enabledCacheModes);
    for (const cacheMode of failureCacheModes) {
      if (!env.CACHE_ENABLED) {
        await assertRequestCacheThrowsError(cacheMode);
        await assertFetchCacheRejectsError(cacheMode);
      } else {
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
    }
    for (const cacheMode of enabledCacheModes) {
      {
        const req = new Request('https://example.org', { cache: cacheMode });
        assert.strictEqual(req.cache, cacheMode);
      }
      {
        const response = await env.SERVICE.fetch(
          'http://placeholder/not-found',
          { cache: cacheMode }
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
    }
  },
};
