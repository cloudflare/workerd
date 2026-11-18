// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A read-through cache in front of read-through-origin.js, as Cloudflare's
// Workers Caching: hits skip the Worker, its headers decide what is stored,
// concurrent misses run it once, and ctx.cache.purge() removes entries.

import assert from 'node:assert';

async function get(env, path, init) {
  const res = await env.CACHED.fetch(`http://example.com${path}`, init);
  return {
    status: res.headers.get('cf-cache-status'),
    body: await res.text(),
    res,
  };
}
const runs = async (env) =>
  Number(await (await env.ORIGIN.fetch('http://example.com/runs')).text());
const purge = async (env, options) =>
  (
    await env.ORIGIN.fetch('http://example.com/purge', {
      method: 'POST',
      body: JSON.stringify(options),
    })
  ).json();

export const aHitSkipsTheWorker = {
  async test(ctrl, env) {
    const before = await runs(env);
    const miss = await get(env, '/hello?x=1');
    assert.strictEqual(miss.status, 'MISS');
    assert.strictEqual(miss.res.headers.get('cache-tag'), null);
    const hit = await get(env, '/hello?x=1');
    assert.deepStrictEqual([hit.status, hit.body], ['HIT', miss.body]);
    assert.strictEqual(await runs(env), before + 1);
    // The query is in the key; the host is not.
    assert.strictEqual((await get(env, '/hello?x=2')).status, 'MISS');
    const other = await env.CACHED.fetch('http://other.test/hello?x=1');
    assert.strictEqual(other.headers.get('cf-cache-status'), 'HIT');
    const head = await env.CACHED.fetch('http://example.com/hello?x=1', {
      method: 'HEAD',
    });
    assert.strictEqual(head.headers.get('cf-cache-status'), 'HIT');
  },
};

export const theResponseHeadersDecide = {
  async test(ctrl, env) {
    const before = await runs(env);
    assert.strictEqual((await get(env, '/no-store')).status, 'BYPASS');
    assert.strictEqual((await get(env, '/no-store')).status, 'BYPASS');
    assert.strictEqual(await runs(env), before + 2);
    // CDN-Cache-Control wins over Cache-Control, which the client still sees.
    await get(env, '/cdn');
    const cdn = await get(env, '/cdn');
    assert.strictEqual(cdn.status, 'HIT');
    assert.strictEqual(cdn.res.headers.get('cache-control'), 'no-store');
    // Expired, it misses.
    assert.strictEqual((await get(env, '/short')).status, 'MISS');
    await scheduler.wait(1_200);
    assert.strictEqual((await get(env, '/short')).status, 'MISS');
  },
};

export const otherMethodsGoToTheWorker = {
  async test(ctrl, env) {
    const before = await runs(env);
    const a = await get(env, '/post', { method: 'POST', body: 'x' });
    const b = await get(env, '/post', { method: 'POST', body: 'x' });
    assert.deepStrictEqual([a.status, b.status], [null, null]);
    assert.strictEqual(await runs(env), before + 2);
  },
};

export const concurrentMissesRunTheWorkerOnce = {
  async test(ctrl, env) {
    const before = await runs(env);
    const all = await Promise.all(
      Array.from({ length: 10 }, () => get(env, '/slow'))
    );
    assert.strictEqual(await runs(env), before + 1);
    const statuses = all.map((r) => r.status).sort();
    assert.deepStrictEqual(statuses, [...Array(9).fill('HIT'), 'MISS']);
    assert(all.every((r) => r.body === all[0].body));
  },
};

export const ctxCachePurgeRemovesEntries = {
  async test(ctrl, env) {
    const paths = ['/tagged/a', '/tagged/b', '/p/a', '/p/b', '/q'];
    const fill = () => Promise.all(paths.map((p) => get(env, p)));
    const statuses = async () =>
      (await Promise.all(paths.map((p) => get(env, p)))).map((r) => r.status);
    await fill();
    assert.deepStrictEqual(await purge(env, { tags: ['two'] }), {
      success: true,
      errors: [],
    });
    assert.deepStrictEqual(await statuses(), [
      'MISS',
      'MISS',
      'HIT',
      'HIT',
      'HIT',
    ]);
    await purge(env, { pathPrefixes: ['/p/'] });
    assert.deepStrictEqual(await statuses(), [
      'HIT',
      'HIT',
      'MISS',
      'MISS',
      'HIT',
    ]);
    await purge(env, { purgeEverything: true });
    assert.deepStrictEqual(await statuses(), Array(5).fill('MISS'));
  },
};

export const aFillBegunBeforeAPurgeStoresNothingItMatches = {
  async test(ctrl, env) {
    const during = async (path, tags) => {
      const filling = get(env, path);
      await scheduler.wait(50);
      await purge(env, { tags });
      return (await filling).body;
    };
    // The Worker ran before the purge: what it said may be what was purged.
    const purged = await during('/slow?a', ['all']);
    const after = await get(env, '/slow?a');
    assert.strictEqual(after.status, 'MISS');
    assert.notStrictEqual(after.body, purged);
    const kept = await during('/slow?b', ['other']);
    assert.deepStrictEqual(Object.values(await get(env, '/slow?b')).slice(0, 2), [
      'HIT',
      kept,
    ]);
  },
};
