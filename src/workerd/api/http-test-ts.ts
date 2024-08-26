// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

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
    let allowedCacheModes: Array<RequestCache> = [
      'default',
      'force-cache',
      'no-cache',
      'no-store',
      'only-if-cached',
      'reload',
    ];
    assert.strictEqual('cache' in Request.prototype, env.CACHE_ENABLED);
    {
      const req = new Request('https://example.org', {});
      assert.strictEqual(req.cache, undefined);
    }
    if (!env.CACHE_ENABLED) {
      for (var cacheMode of allowedCacheModes) {
        await assertRequestCacheThrowsError(cacheMode);
        await assertFetchCacheRejectsError(cacheMode);
      }
    } else {
      for (var cacheMode of allowedCacheModes) {
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
  },
};
