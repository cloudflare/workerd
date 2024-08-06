// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from "node:assert";

async function assertRequestCacheThrowsError(cacheHeader: RequestCache,
  errorName: String = 'Error',
  errorMessage: String = "The 'cache' field on 'RequestInitializerDict' is not implemented.") {
  assert.throws(() => {
    const header = { cache : cacheHeader};
    const req: RequestInit = header;
    new Request('https://example.org', req);
  }, {
    name: errorName,
    message: errorMessage,
  });
}

async function assertFetchCacheRejectsError(cacheHeader: RequestCache,
  errorName: String = 'Error',
  errorMessage: String = "The 'cache' field on 'RequestInitializerDict' is not implemented.") {
  await assert.rejects((async () => {
    const header = { cache : cacheHeader};
    const req: RequestInit = header;
    await fetch('http://example.org', req);
  })(), {
    name: errorName,
    message: errorMessage,
  });
}

export const cacheMode = {

  async test() {
    assert.strictEqual("cache" in Request.prototype, false);
    {
      const req = new Request('https://example.org', {});
      assert.strictEqual(req.cache, undefined);
    }
    assertRequestCacheThrowsError('default');
    assertRequestCacheThrowsError('force-cache');
    assertRequestCacheThrowsError('no-cache');
    assertRequestCacheThrowsError('no-store');
    assertRequestCacheThrowsError('only-if-cached');
    assertRequestCacheThrowsError('reload');
    assertFetchCacheRejectsError('default');
    assertFetchCacheRejectsError('force-cache');
    assertFetchCacheRejectsError('no-cache');
    assertFetchCacheRejectsError('no-store');
    assertFetchCacheRejectsError('only-if-cached');
    assertFetchCacheRejectsError('reload');
  }
}
