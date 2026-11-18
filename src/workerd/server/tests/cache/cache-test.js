// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// /workerd/cache.js on a disk, through `caches`: Miniflare's
// test/plugins/cache/index.spec.ts, ported, and how long a response stays
// fresh (`lifetimes`, on the cache Worker's TEST_CLOCK; `expires` in real
// time).

import assert from 'node:assert';

const HOUR = { 'Cache-Control': 'max-age=3600' };
const url = (name) => `http://localhost/${name}`;
const bytes = (text) => new TextEncoder().encode(text);

export const matchHit = {
  async test(ctrl, env) {
    const cache = caches.default;
    const key = url('hit');
    let headers = { ...HOUR, 'X-Key': 'value' };
    await cache.put(key, new Response('body', { headers }));
    let res = await cache.match(key);
    assert.strictEqual(res.status, 200);
    assert.strictEqual(res.headers.get('Cache-Control'), 'max-age=3600');
    assert.strictEqual(res.headers.get('CF-Cache-Status'), 'HIT');
    assert.strictEqual(res.headers.get('X-Key'), 'value');
    assert.strictEqual(await res.text(), 'body');

    const bytes = new Uint8Array([1, 2, 3]);
    await cache.put(key, new Response(bytes, { headers: HOUR }));
    res = await cache.match(key);
    assert.deepStrictEqual(new Uint8Array(await res.arrayBuffer()), bytes);

    const big = 'x'.repeat(1024 * 1024);
    await cache.put(key, new Response(big, { headers: HOUR }));
    assert.strictEqual(await (await cache.match(key)).text(), big);

    // A 404 is cached and returned as one.
    headers = HOUR;
    await cache.put(key, new Response('gone', { status: 404, headers }));
    res = await cache.match(key);
    assert.strictEqual(res.status, 404);
    assert.strictEqual(await res.text(), 'gone');
  },
};

export const matchEmpty = {
  async test() {
    const key = url('empty');
    await caches.default.put(key, new Response(null, { headers: HOUR }));
    const res = await caches.default.match(key);
    assert.strictEqual(res.status, 200);
    assert.strictEqual(await res.text(), '');
  },
};

export const matchMiss = {
  async test() {
    assert.strictEqual(await caches.default.match(url('miss')), undefined);
  },
};

export const ifNoneMatch = {
  async test() {
    const cache = caches.default;
    const key = url('if-none-match');
    const headers = { ETag: '"thing"', ...HOUR };
    await cache.put(key, new Response('body', { headers }));
    const status = async (value) =>
      (
        await cache.match(
          new Request(key, { headers: { 'If-None-Match': value } })
        )
      ).status;
    assert.strictEqual(await status('"thing"'), 304);
    assert.strictEqual(await status('   W/"thing"      '), 304);
    assert.strictEqual(await status('"not the thing"'), 200);
    assert.strictEqual(
      await status('"not the thing",    "thing"    , W/"still not"'),
      304
    );
    assert.strictEqual(await status('*'), 304);
    assert.strictEqual(await status('    *   '), 304);
  },
};

export const ifModifiedSince = {
  async test() {
    const cache = caches.default;
    const key = url('if-modified-since');
    const headers = { 'Last-Modified': 'Tue, 13 Sep 2022 12:00:00 GMT' };
    Object.assign(headers, HOUR);
    await cache.put(key, new Response('body', { headers }));
    const status = async (value) =>
      (
        await cache.match(
          new Request(key, { headers: { 'If-Modified-Since': value } })
        )
      ).status;
    assert.strictEqual(await status('Tue, 13 Sep 2022 11:00:00 GMT'), 200);
    assert.strictEqual(await status('Tue, 13 Sep 2022 12:00:00 GMT'), 304);
    assert.strictEqual(await status('Tue, 13 Sep 2022 13:00:00 GMT'), 304);
    assert.strictEqual(await status('13 Sep 2022 13:00:00 GMT'), 200);
  },
};

export const ranges = {
  async test() {
    const cache = caches.default;
    const key = url('range');
    const headers = { 'Content-Type': 'text/plain', ...HOUR };
    await cache.put(key, new Response('0123456789', { headers }));
    const match = (range) =>
      cache.match(new Request(key, { headers: { Range: range } }));

    let res = await match('bytes=2-4');
    assert.strictEqual(res.status, 206);
    assert.strictEqual(res.headers.get('Content-Length'), '3');
    assert.strictEqual(res.headers.get('Content-Range'), 'bytes 2-4/10');
    assert.strictEqual(res.headers.get('Cache-Control'), 'max-age=3600');
    assert.strictEqual(res.headers.get('CF-Cache-Status'), 'HIT');
    assert.strictEqual(await res.text(), '234');

    res = await match('bytes=-3');
    assert.strictEqual(await res.text(), '789');

    res = await match('bytes=1-3,5-6');
    assert.strictEqual(res.status, 206);
    assert.strictEqual(res.headers.get('CF-Cache-Status'), 'HIT');
    const [brand, boundary] = res.headers.get('Content-Type').split('=');
    assert.strictEqual(brand, 'multipart/byteranges; boundary');
    assert.strictEqual(
      await res.text(),
      [
        `--${boundary}`,
        'Content-Type: text/plain',
        'Content-Range: bytes 1-3/10',
        '',
        '123',
        `--${boundary}`,
        'Content-Type: text/plain',
        'Content-Range: bytes 5-6/10',
        '',
        '56',
        `--${boundary}--`,
      ].join('\r\n')
    );

    res = await match('bytes=15-');
    assert.strictEqual(res.status, 416);
    assert.strictEqual(res.headers.get('Content-Range'), 'bytes */10');
  },
};

export const overwrite = {
  async test(ctrl, env) {
    const cache = caches.default;
    const key = url('overwrite');
    await cache.put(key, new Response('body1', { headers: HOUR }));
    await cache.put(key, new Response('body2', { headers: HOUR }));
    assert.strictEqual(await (await cache.match(key)).text(), 'body2');
  },
};

// Fresh for a tenth of the time since Last-Modified, 1 s, then a miss, then
// gone.
export const expires = {
  async test() {
    const cache = caches.default;
    const key = url('expires');
    const now = Math.floor(Date.now() / 1000) * 1000;
    const headers = {
      Date: new Date(now).toUTCString(),
      'Last-Modified': new Date(now - 10_000).toUTCString(),
    };
    await cache.put(key, new Response('body', { headers }));
    assert.strictEqual((await cache.match(key)).status, 200);
    await scheduler.wait(1_200);
    assert.strictEqual(await cache.match(key), undefined);
    assert.strictEqual(await cache.delete(key), false);
  },
};

// Whether a response with `headers` and an Expires is cached.
async function isCached(name, headers, status = 200) {
  const key = url(`is-cached-${name}`);
  const now = Math.floor(Date.now() / 1000) * 1000;
  const all = {
    Date: new Date(now).toUTCString(),
    Expires: new Date(now + 60_000).toUTCString(),
    ...headers,
  };
  const res = new Response('body', { status, headers: all });
  await caches.default.put(key, res);
  return (await caches.default.match(key)) ?? null;
}

export const notCached = {
  async test() {
    const no = async (name, headers, status) =>
      assert.strictEqual(await isCached(name, headers, status), null, name);
    await no('private', { 'Cache-Control': 'private' });
    await no('no-store', { 'Cache-Control': 'no-store' });
    await no('no-cache', { 'Cache-Control': 'no-cache' });
    await no('set-cookie', { 'Set-Cookie': 'key=value' });
    await no('status-500', {}, 500);
    // No freshness information at all.
    const key = url('no-freshness');
    await caches.default.put(key, new Response('body'));
    assert.strictEqual(await caches.default.match(key), undefined);
  },
};

export const privateSetCookie = {
  async test() {
    const res = await isCached('private-set-cookie', {
      'Cache-Control': 'private=Set-Cookie, max-age=60',
      'Set-Cookie': 'key=value',
    });
    assert.strictEqual(res.status, 200);
    assert.strictEqual(res.headers.get('Set-Cookie'), null);
    assert.strictEqual(res.headers.get('Cache-Control'), 'max-age=60');
  },
};

// A response fresh for no time drops what was there, as Miniflare's
// immediately expired entry does; one it refuses keeps it.
export const replacedByUnstorable = {
  async test() {
    const key = url('replace-no-cache');
    await caches.default.put(key, new Response('old', { headers: HOUR }));
    let headers = { 'Cache-Control': 'private' };
    await caches.default.put(key, new Response('new', { headers }));
    assert.strictEqual(await (await caches.default.match(key)).text(), 'old');
    headers = { 'Cache-Control': 'no-cache' };
    await caches.default.put(key, new Response('new', { headers }));
    assert.strictEqual(await caches.default.match(key), undefined);
  },
};

export const deleteReturns = {
  async test() {
    const key = url('delete');
    await caches.default.put(key, new Response('body', { headers: HOUR }));
    assert.strictEqual(await caches.default.delete(key), true);
    assert.strictEqual(await caches.default.delete(key), false);
  },
};

export const cfCacheKey = {
  async test() {
    const cache = caches.default;
    const key = url('cf-key-unused');
    const key1 = new Request(key, { cf: { cacheKey: '1' } });
    const key2 = new Request(key, { cf: { cacheKey: '2' } });
    await cache.put(key1, new Response('body1', { headers: HOUR }));
    await cache.put(key2, new Response('body2', { headers: HOUR }));
    assert.strictEqual(await (await cache.match(key1)).text(), 'body1');
    assert.strictEqual(await (await cache.match(key2)).text(), 'body2');
    assert.strictEqual(await cache.match(key), undefined);
    assert.strictEqual(await cache.delete(key1), true);
    assert.strictEqual(await cache.delete(key2), true);
  },
};

export const namedDisjoint = {
  async test() {
    const key = url('disjoint');
    const one = await caches.open('1');
    const two = await caches.open('2');
    await caches.default.put(key, new Response('d', { headers: HOUR }));
    await one.put(key, new Response('1', { headers: HOUR }));
    await two.put(key, new Response('2', { headers: HOUR }));
    assert.strictEqual(await (await caches.default.match(key)).text(), 'd');
    assert.strictEqual(await (await one.match(key)).text(), '1');
    assert.strictEqual(await (await two.match(key)).text(), '2');
    assert.strictEqual(await caches.default.delete(key), true);
    assert.strictEqual(await one.delete(key), true);
    assert.strictEqual(await two.delete(key), true);
  },
};

// PUTs straight to the service: one whose body is not its Content-Length is
// not stored; one over 512 MiB is refused.
export const truncatedPut = {
  async test(ctrl, env) {
    const key = url('truncated');
    const put = async (body) =>
      (await env.CACHE.fetch(key, { method: 'PUT', body })).status;
    const head = (length) =>
      'HTTP/1.1 200 OK\r\nCache-Control: max-age=3600\r\n' +
      `Content-Length: ${length}\r\n\r\n`;
    assert.strictEqual(await put(head(10) + 'abc'), 400);
    assert.strictEqual(await put(head(2) + 'abc'), 400);
    assert.strictEqual(await caches.default.match(key), undefined);
    assert.strictEqual(await put(head(600 * 1024 * 1024) + 'abc'), 413);
    assert.strictEqual(await caches.default.match(key), undefined);
    assert.strictEqual(await put('HTTP/1.1 200 OK\r\n'), 400);
    assert.strictEqual(await put('HTTP/1.1 2000 OK\r\n\r\n'), 400);
    const header = (line) => put(`HTTP/1.1 200 OK\r\n${line}\r\n\r\n`);
    assert.strictEqual(await header('no colon'), 400);
    assert.strictEqual(await header('bad name: x'), 400);
    assert.strictEqual(await put(head(3) + 'abc'), 204);
    assert.strictEqual(await (await caches.default.match(key)).text(), 'abc');
  },
};

// A put whose body fails stores nothing, and keeps what was there: with no
// length, and with one (a FixedLengthStream's).
export const failedPut = {
  async test() {
    const cache = caches.default;
    const key = url('failed-put');
    await cache.put(key, new Response('old', { headers: HOUR }));
    const body = new ReadableStream({
      start: (c) => c.enqueue(bytes('partial')),
      pull: (c) => c.error(new Error('boom')),
    });
    await assert.rejects(
      cache.put(key, new Response(body, { headers: HOUR })),
      /boom/
    );
    assert.strictEqual(await (await cache.match(key)).text(), 'old');

    const { readable, writable } = new FixedLengthStream(10);
    const writer = writable.getWriter();
    const put = cache.put(key, new Response(readable, { headers: HOUR }));
    await writer.write(bytes('abc'));
    await writer.abort(new Error('boom'));
    await assert.rejects(put, /boom/);
    assert.strictEqual(await (await cache.match(key)).text(), 'old');
  },
};

// Each case puts 'old' for an hour, then 'new' with `headers()`, which live
// `ms`: 0 deletes 'old', null stores nothing and keeps it. As
// http-cache-semantics decides for Miniflare.
export const lifetimes = {
  async test(ctrl, env) {
    let skew = 0;
    const advance = async (ms) => {
      skew += ms;
      const at = `http://cache/clock?advance=${ms}`;
      assert.ok((await env.CACHE.fetch(at, { method: 'POST' })).ok);
    };
    // An HTTP date `ms` from the cache's clock, in whole seconds.
    const date = (ms) => new Date(Date.now() + skew + ms).toUTCString();
    const cc = (value) => () => ({ 'Cache-Control': value });
    const hour = 3_600_000;
    const cases = [
      ['s-maxage', 1000, cc('s-maxage=1, max-age=10')],
      ['age', 6000, () => ({ 'Cache-Control': 'max-age=10', Age: '4' })],
      ['too-old', 0, () => ({ 'Cache-Control': 'max-age=10', Age: '40' })],
      ['quoted', 10_000, cc('max-age="10s"')],
      ['bare', 0, cc('max-age')],
      ['no-cache', 0, cc('max-age=60, no-cache')],
      ['expires', 2000, () => ({ Date: date(-5000), Expires: date(-3000) })],
      ['expires-undated', 2000, () => ({ Expires: date(2000) })],
      ['expired', 0, () => ({ Expires: date(-2000) })],
      ['expires-0', 0, () => ({ Expires: '0' })],
      [
        'last-modified',
        2000,
        () => ({ Date: date(0), 'Last-Modified': date(-20_000) }),
      ],
      [
        'last-modified-undated',
        2000,
        () => ({ 'Last-Modified': date(-20_000) }),
      ],
      [
        'immutable',
        24 * hour,
        () => ({
          'Cache-Control': 'immutable',
          'Last-Modified': date(-20_000),
        }),
      ],
      ['none', 0, () => ({})],
      ['pragma', 0, () => ({ Pragma: 'no-cache', Expires: date(2000) })],
      ['private', null, cc('private')],
      ['no-store', null, cc('no-store, max-age=60')],
      ['set-cookie', null, () => ({ ...HOUR, 'Set-Cookie': 'key=value' })],
      ['500', null, () => HOUR, { status: 500 }],
      ['302', hour, () => HOUR, { status: 302 }],
      [
        '308',
        2000,
        () => ({ 'Last-Modified': date(-20_000) }),
        { status: 308 },
      ],
      ['404', 0, () => ({}), { status: 404 }],
      ['authorization', null, () => HOUR, { auth: true }],
      ['public', 60_000, cc('public, max-age=60'), { auth: true }],
    ];
    const cache = caches.default;
    for (const [name, ms, headers, { status = 200, auth } = {}] of cases) {
      const key = url(`lifetime-${name}`);
      await cache.put(key, new Response('old', { headers: HOUR }));
      const init = auth ? { headers: { Authorization: 'x' } } : {};
      const res = new Response('new', { status, headers: headers() });
      await cache.put(new Request(key, init), res);
      const text = async () => (await cache.match(key))?.text();
      if (!ms) {
        assert.strictEqual(await text(), ms === null ? 'old' : undefined, name);
        continue;
      }
      assert.strictEqual(await text(), 'new', name);
      await advance(ms / 4);
      assert.strictEqual(await text(), 'new', name);
      await advance((ms * 3) / 4 + 500);
      assert.strictEqual(await text(), undefined, name);
    }
  },
};

// Hop-by-hop headers are not stored, nor a cookie `private=set-cookie` names.
// (`Connection`, and so the headers it names, never reaches the cache.)
export const storedHeaders = {
  async test() {
    const key = url('stored-headers');
    const headers = {
      'Cache-Control': 'max-age=60, private=set-cookie',
      'Set-Cookie': 'key=value',
      'Keep-Alive': 'timeout=5',
      'X-Kept': '2',
    };
    await caches.default.put(key, new Response('body', { headers }));
    const res = await caches.default.match(key);
    assert.strictEqual(res.headers.get('Cache-Control'), 'max-age=60');
    assert.strictEqual(res.headers.get('X-Kept'), '2');
    assert.strictEqual(res.headers.get('Set-Cookie'), null);
    assert.strictEqual(res.headers.get('Keep-Alive'), null);
  },
};

// An entry is a record on the disk at the SHA-256 of `<namespace>\0<key>`:
// a 4-byte length, that much JSON, then the body.
export const storeKey = {
  async test(ctrl, env) {
    const key = url('store-key');
    await caches.default.put(key, new Response('body', { headers: HOUR }));
    const digest = await crypto.subtle.digest('SHA-256', bytes(`\0${key}`));
    const hex = Array.from(new Uint8Array(digest), (b) =>
      b.toString(16).padStart(2, '0')
    ).join('');
    const r = await env.DISK.fetch(`http://d/${hex}`);
    assert.strictEqual(r.status, 200);
    const record = new Uint8Array(await r.arrayBuffer());
    const n = new DataView(record.buffer).getUint32(0);
    const head = JSON.parse(new TextDecoder().decode(record.slice(4, 4 + n)));
    assert.strictEqual(head.status, 200);
    assert.ok(head.expires > Date.now());
    assert.strictEqual(new TextDecoder().decode(record.slice(4 + n)), 'body');
    // Not a record: a miss, and deleted.
    await env.DISK.fetch(`http://d/${hex}`, { method: 'PUT', body: 'junk' });
    assert.strictEqual(await caches.default.match(key), undefined);
    assert.strictEqual((await env.DISK.fetch(`http://d/${hex}`)).status, 404);
  },
};
