// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

const now = () => Math.floor(Date.now() / 1000);

// The blob files of every namespace.
async function blobs(env) {
  const ls = async (path) => {
    const r = await env.DISK.fetch(`http://d/${path}`);
    return r.ok ? (await r.json()).map((e) => e.name) : [];
  };
  const out = [];
  for (const dir of await ls('')) out.push(...(await ls(`${dir}/b/`)));
  return out.sort();
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export const getPutDelete = {
  async test(ctrl, env) {
    assert.strictEqual(await env.KV.get('missing'), null);
    await env.KV.put('k', 'v');
    assert.strictEqual(await env.KV.get('k'), 'v');
    assert.strictEqual(await env.SAME.get('k'), 'v');
    assert.strictEqual(await env.OTHER.get('k'), null);
    await env.KV.put('k', new Uint8Array([1, 2, 3]));
    const bytes = await env.KV.get('k', 'arrayBuffer');
    assert.deepStrictEqual(new Uint8Array(bytes), new Uint8Array([1, 2, 3]));
    await env.KV.put('j', JSON.stringify({ a: [1] }));
    assert.deepStrictEqual(await env.KV.get('j', 'json'), { a: [1] });
    const big = 'x'.repeat(1024 * 1024);
    await env.KV.put('big', big);
    assert.strictEqual(await env.KV.get('big'), big);
    const odd = 'a/b c?d=e&f%20é/..';
    await env.KV.put(odd, 'odd');
    assert.strictEqual(await env.KV.get(odd), 'odd');
    await env.KV.delete('k');
    await env.KV.delete('never-there');
    assert.strictEqual(await env.KV.get('k'), null);
  },
};

// A value written as a stream, in chunks, inline and as a blob.
export const streamedPut = {
  async test(ctrl, env) {
    for (const size of [1000, 70_000, 2 * 1024 * 1024]) {
      const value = Uint8Array.from({ length: size }, (_, i) => i % 251);
      let sent = 0;
      const stream = new ReadableStream({
        pull(c) {
          c.enqueue(value.slice(sent, (sent += 16_384)));
          if (sent >= size) c.close();
        },
      });
      await env.KV.put(`stream/${size}`, stream);
      const got = await env.KV.get(`stream/${size}`, 'arrayBuffer');
      assert.deepStrictEqual(new Uint8Array(got), value);
    }
  },
};

export const metadata = {
  async test(ctrl, env) {
    await env.KV.put('m', 'v', { metadata: { n: 1, s: 'é' } });
    const got = await env.KV.getWithMetadata('m');
    assert.deepStrictEqual(got.value, 'v');
    assert.deepStrictEqual(got.metadata, { n: 1, s: 'é' });
    await env.KV.put('m', 'w');
    assert.strictEqual((await env.KV.getWithMetadata('m')).metadata, null);
    await assert.rejects(
      env.KV.put('m', 'v', { metadata: { big: 'x'.repeat(2000) } }),
      /413/
    );
  },
};

export const expiration = {
  async test(ctrl, env) {
    await env.KV.put('e/ttl', 'v', { expirationTtl: 60 });
    await env.KV.put('e/at', 'v', { expiration: now() + 3600 });
    assert.strictEqual(await env.KV.get('e/ttl'), 'v');
    const { keys } = await env.KV.list({ prefix: 'e/' });
    assert.strictEqual(keys.length, 2);
    const at = keys.find((k) => k.name === 'e/at').expiration;
    const ttl = keys.find((k) => k.name === 'e/ttl').expiration;
    assert.ok(Math.abs(at - (now() + 3600)) <= 2, `${at}`);
    assert.ok(Math.abs(ttl - (now() + 60)) <= 2, `${ttl}`);
    await assert.rejects(env.KV.put('e/x', 'v', { expirationTtl: 10 }), /400/);
    await assert.rejects(
      env.KV.put('e/x', 'v', { expiration: now() - 10 }),
      /400/
    );
  },
};

export const list = {
  async test(ctrl, env) {
    for (const k of ['p/a', 'p/b', 'p/c', 'p/d', 'p/e', 'q']) {
      await env.KV.put(k, '', { metadata: { k } });
    }
    let page = await env.KV.list({ prefix: 'p/', limit: 2 });
    assert.deepStrictEqual(
      page.keys.map((k) => k.name),
      ['p/a', 'p/b']
    );
    assert.deepStrictEqual(page.keys[0].metadata, { k: 'p/a' });
    assert.strictEqual(page.list_complete, false);
    const names = page.keys.map((k) => k.name);
    while (!page.list_complete) {
      page = await env.KV.list({ prefix: 'p/', limit: 2, cursor: page.cursor });
      names.push(...page.keys.map((k) => k.name));
    }
    assert.deepStrictEqual(names, ['p/a', 'p/b', 'p/c', 'p/d', 'p/e']);
    const all = await env.OTHER.list();
    assert.deepStrictEqual(all.keys, []);
    assert.strictEqual(all.list_complete, true);
  },
};

export const bulk = {
  async test(ctrl, env) {
    await env.KV.put('b1', '"one"', { metadata: 1 });
    await env.KV.put('b2', '2');
    const got = await env.KV.get(['b1', 'b2', 'nope']);
    assert.deepStrictEqual(
      [...got],
      [
        ['b1', '"one"'],
        ['b2', '2'],
        ['nope', null],
      ]
    );
    const json = await env.KV.getWithMetadata(['b1', 'b2'], 'json');
    assert.deepStrictEqual(json.get('b1'), { value: 'one', metadata: 1 });
    assert.deepStrictEqual(json.get('b2'), { value: 2, metadata: null });
    // Bytes that are not UTF-8 read as text, as a single get does.
    await env.KV.put('b3', new Uint8Array([0xff, 0x61]));
    assert.strictEqual((await env.KV.get(['b3'])).get('b3'), '\ufffda');
    await assert.rejects(env.KV.get(['b3'], 'json'), /non-json/);
  },
};

// Up to 64 KiB is inline in the index; more is a blob.
export const threshold = {
  async test(ctrl, env) {
    const before = (await blobs(env)).length;
    const at = 'x'.repeat(64 * 1024);
    await env.KV.put('t/at', at);
    assert.strictEqual((await blobs(env)).length, before);
    await env.KV.put('t/over', at + 'y');
    assert.strictEqual((await blobs(env)).length, before + 1);
    assert.strictEqual(await env.OTHER.get('t/over'), null);
    assert.strictEqual(await env.KV.get('t/at'), at);
    assert.strictEqual(await env.KV.get('t/over'), at + 'y');
    await env.KV.delete('t/at');
    await env.KV.delete('t/over');
  },
};

// An overwritten or deleted blob is deleted once its grace period, shortened
// by `TEST_CLOCK`, is over.
export const blobGc = {
  async test(ctrl, env) {
    await sleep(500);
    const before = await blobs(env);
    const big = (c) => c.repeat(100_000);
    await env.KV.put('gc', big('a'));
    await env.KV.put('gc', big('b'));
    assert.strictEqual((await blobs(env)).length, before.length + 2);
    assert.strictEqual(await env.KV.get('gc'), big('b'));
    await sleep(500);
    assert.strictEqual((await blobs(env)).length, before.length + 1);
    await env.KV.put('gc', 'small');
    await env.KV.put('gc2', big('c'));
    await env.KV.delete('gc2');
    await sleep(500);
    assert.deepStrictEqual(await blobs(env), before);
    assert.strictEqual(await env.KV.get('gc'), 'small');
    assert.strictEqual(await env.KV.get('gc2'), null);
  },
};

export const listInlineAndBlobs = {
  async test(ctrl, env) {
    const big = 'z'.repeat(70_000);
    await env.KV.put('mix/a', 'a', { metadata: 'a' });
    await env.KV.put('mix/b', big, { metadata: 'b' });
    await env.KV.put('mix/c', 'c', { metadata: 'c' });
    const page = await env.KV.list({ prefix: 'mix/', limit: 2 });
    assert.deepStrictEqual(page.keys, [
      { name: 'mix/a', metadata: 'a' },
      { name: 'mix/b', metadata: 'b' },
    ]);
    const rest = await env.KV.list({ prefix: 'mix/', cursor: page.cursor });
    assert.deepStrictEqual(rest.keys, [{ name: 'mix/c', metadata: 'c' }]);
    assert.strictEqual(rest.list_complete, true);
    const got = await env.KV.get(['mix/a', 'mix/b']);
    assert.strictEqual(got.get('mix/b'), big);
  },
};

// Locally nothing is cached: another process on the same storage, as
// `wrangler kv key put --local` is, must be seen at once (kv-wrangler-test).
// The cluster's cache is cluster-test's.
export const cacheTtl = {
  async test(ctrl, env) {
    await env.KV.put('c', 'v');
    assert.strictEqual(await env.KV.get('c', { cacheTtl: 30 }), 'v');
    await assert.rejects(
      env.KV.get('c', { cacheTtl: 29 }),
      /400 Invalid cache_ttl of 29. Cache TTL must be at least 30./
    );
    const got = await env.KV.getWithMetadata('c');
    assert.deepStrictEqual([got.value, got.cacheStatus], ['v', 'MISS']);
  },
};

// A read racing a write from the same process.
export const readRacesWrite = {
  async test(ctrl, env) {
    const old = 'o'.repeat(2 * 1024 * 1024);
    await env.KV.put('race', old);
    const [got] = await Promise.all([
      env.KV.get('race'),
      env.KV.put('race', 'new'),
    ]);
    assert.ok(got === old || got === 'new');
    assert.strictEqual(await env.KV.get('race'), 'new');
  },
};

// The errors production gives, as Miniflare's KV tests check them.
export const errors = {
  async test(ctrl, env) {
    const fails = (p, text) =>
      assert.rejects(p, (e) => {
        assert.strictEqual(e.message, text);
        return true;
      });
    const t = now();
    const put = (options) => env.KV.put('k', 'v', options);
    for (const ttl of [0, -5]) {
      await fails(
        put({ expirationTtl: ttl }),
        `KV PUT failed: 400 Invalid expiration_ttl of ${ttl}. Please ` +
          'specify integer greater than 0.'
      );
    }
    await fails(
      put({ expirationTtl: 30 }),
      'KV PUT failed: 400 Invalid expiration_ttl of 30. Expiration TTL ' +
        'must be at least 60.'
    );
    for (const at of [t, -5]) {
      await fails(
        put({ expiration: at }),
        `KV PUT failed: 400 Invalid expiration of ${at}. Please specify ` +
          'integer greater than the current number of seconds since the ' +
          'UNIX epoch.'
      );
    }
    await fails(
      put({ expiration: t + 30 }),
      `KV PUT failed: 400 Invalid expiration of ${t + 30}. Expiration ` +
        'times must be at least 60 seconds in the future.'
    );
    const max = 25 * 1024 * 1024;
    await fails(
      env.KV.put('k', new Uint8Array(max + 1)),
      `KV PUT failed: 413 Value length of ${max + 1} exceeds limit of ${max}.`
    );
    // Metadata counts in UTF-8 bytes: "é…é" is 2 + 2n of them.
    await put({ metadata: 'é'.repeat(511) });
    await fails(
      put({ metadata: 'é'.repeat(512) }),
      'KV PUT failed: 413 Metadata length of 1026 exceeds limit of 1024.'
    );
    await fails(
      env.KV.list({ limit: 1001 }),
      'KV GET failed: 400 Invalid key_count_limit of 1001. Please specify ' +
        'an integer less than 1000.'
    );
    await fails(
      env.KV.list({ prefix: 'p'.repeat(513) }),
      'KV GET failed: 414 UTF-8 encoded length of 513 exceeds key length ' +
        'limit of 512.'
    );
    const keys = Array.from({ length: 101 }, (_, i) => `k${i}`);
    await fails(
      env.KV.get(keys),
      'KV GET_BULK failed: 400 You can request a maximum of 100 keys'
    );
    await fails(
      env.KV.get([]),
      'KV GET_BULK failed: 400 You must request a minimum of 1 key'
    );
    await fails(
      env.KV.get(['a', '']),
      'KV GET_BULK failed: 400 Key names must not be empty'
    );
    await fails(
      env.KV.get(['a', '..']),
      'KV GET_BULK failed: 400 Illegal key name "..". Please use a ' +
        'different name.'
    );
    await fails(
      env.KV.get(['a', 'k'.repeat(513)]),
      'KV GET_BULK failed: 414 UTF-8 encoded length of 513 exceeds key ' +
        'length limit of 512.'
    );
    const half = 'x'.repeat(13 * 1024 * 1024);
    await env.KV.put('half1', half);
    await env.KV.put('half2', half);
    await fails(
      env.KV.get(['half1', 'half2']),
      'KV GET_BULK failed: 413 Total size of request exceeds the limit of ' +
        '25MB'
    );
    assert.strictEqual((await env.KV.get(['half1'])).get('half1'), half);
  },
};

// A malformed key escape or a bulk body that isn't an object is a 400.
export const malformedRequests = {
  async test(ctrl, env) {
    const bad = await env.KV_WORKER.fetch('http://kv/%E0%A4%A');
    assert.strictEqual(bad.status, 400);
    const bulk = await env.KV_WORKER.fetch('http://kv/bulk/get', {
      method: 'POST',
      body: 'null',
    });
    assert.strictEqual(bulk.status, 400);
  },
};

// Keys disappear from get and list once the expiration clock passes them.
export const expiry = {
  async test(ctrl, env) {
    await env.KV.put('x/ttl', 'v', { expirationTtl: 60 });
    await env.KV.put('x/at', 'v', { expiration: now() + 120 });
    await env.KV.put('x/forever', 'v');
    const advance = (s) =>
      env.KV_WORKER.fetch(`http://kv/clock?advance=${s * 1000}`, {
        method: 'POST',
      });
    try {
      await advance(61);
      assert.strictEqual(await env.KV.get('x/ttl'), null);
      assert.strictEqual(await env.KV.get('x/at'), 'v');
      let { keys } = await env.KV.list({ prefix: 'x/' });
      assert.deepStrictEqual(
        keys.map((k) => k.name),
        ['x/at', 'x/forever']
      );
      await advance(60);
      assert.strictEqual(await env.KV.get('x/at'), null);
      ({ keys } = await env.KV.list({ prefix: 'x/' }));
      assert.deepStrictEqual(
        keys.map((k) => k.name),
        ['x/forever']
      );
    } finally {
      await advance(-121);
    }
  },
};

// A key whose name is a URL's special characters.
export const keyNames = {
  async test(ctrl, env) {
    for (const key of ['a/../b', '.a', '?', '#', '%', ' ', 'bulk/get']) {
      await env.KV.put(key, key);
      assert.strictEqual(await env.KV.get(key), key);
    }
  },
};
