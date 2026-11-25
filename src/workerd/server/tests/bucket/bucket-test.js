// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The `bucket` service's HTTP API (services/bucket.rs), over its local
// store.

import assert from 'node:assert';

const url = (key, query = '') =>
  `http://b/${encodeURIComponent(key)}${query && `?${query}`}`;

async function put(bucket, key, body, headers = {}) {
  return bucket.fetch(url(key), { method: 'PUT', body, headers });
}

async function refusal(response, status, code) {
  assert.strictEqual(response.status, status);
  assert.strictEqual((await response.json()).code, code);
}

// `n` bytes, the same each time: each 8 KiB block is a slice of RUN starting
// at the block's index modulo 2048. RUN repeats only every 64 KiB, so blocks
// differ for 16 MiB and the parts of a multipart write differ from each
// other. Streams are copied together from RUN rather than computed a byte at
// a time: tens of MiB of per-byte JavaScript take tens of seconds in a debug
// build whenever V8 leaves the loop unoptimized.
const BLOCK = 8192;
const OFFSETS = 2048;
const RUN = Uint8Array.from(
  { length: BLOCK + OFFSETS },
  (_, m) => (m * 7 + (m >> 8)) & 0xff
);

function pattern(n, chunk = 1 << 20) {
  let sent = 0;
  return new ReadableStream({
    pull(controller) {
      if (sent === n) return controller.close();
      const bytes = new Uint8Array(Math.min(chunk, n - sent));
      for (let at = 0; at < bytes.length;) {
        const i = sent + at;
        const inBlock = i % BLOCK;
        const start = inBlock + (Math.floor(i / BLOCK) % OFFSETS);
        const length = Math.min(BLOCK - inBlock, bytes.length - at);
        bytes.set(RUN.subarray(start, start + length), at);
        at += length;
      }
      sent += bytes.length;
      controller.enqueue(bytes);
    },
  });
}

async function digest(stream) {
  const bytes = new Uint8Array(await new Response(stream).arrayBuffer());
  return [bytes.length, await crypto.subtle.digest('SHA-256', bytes)];
}

export const ranges = {
  async test(ctrl, env) {
    const B = env.BUCKET;
    assert.strictEqual((await put(B, 'r', '0123456789')).status, 200);
    const get = (range) =>
      B.fetch(url('r'), { headers: range ? { range } : {} });
    const whole = await get();
    assert.strictEqual(whole.status, 200);
    assert.strictEqual(whole.headers.get('content-length'), '10');
    assert.strictEqual(await whole.text(), '0123456789');
    for (const [range, body, served] of [
      ['bytes=2-4', '234', 'bytes 2-4/10'],
      ['bytes=8-', '89', 'bytes 8-9/10'],
      ['bytes=-3', '789', 'bytes 7-9/10'],
      ['bytes=5-100', '56789', 'bytes 5-9/10'],
      ['bytes=5-18446744073709551615', '56789', 'bytes 5-9/10'],
    ]) {
      const r = await get(range);
      assert.strictEqual(r.status, 206, range);
      assert.strictEqual(r.headers.get('content-range'), served);
      assert.strictEqual(r.headers.get('content-length'), `${body.length}`);
      assert.strictEqual(await r.text(), body);
    }
    await refusal(await get('bytes=10-'), 416, 'InvalidRange');
    await refusal(await get('bytes=4-2'), 400, 'InvalidArgument');
    await refusal(await B.fetch(url('absent')), 404, 'NoSuchKey');
    const head = await B.fetch(url('r'), { method: 'HEAD' });
    assert.strictEqual(head.status, 200);
    assert.strictEqual(head.headers.get('content-length'), '10');
    const absent = await B.fetch(url('absent'), { method: 'HEAD' });
    assert.strictEqual(absent.status, 404);
  },
};

export const metadata = {
  async test(ctrl, env) {
    const B = env.BUCKET;
    const before = Date.now();
    const written = await put(B, 'm', 'hello', {
      'content-type': 'text/x-test',
      'content-language': 'en',
      'content-disposition': 'attachment',
      'cache-control': 'no-store',
      'x-meta-Color': 'blue',
      'x-meta-escaped': '%E2%9C%93',
    });
    assert.strictEqual(written.status, 200);
    // MD5 of "hello".
    const etag = '"5d41402abc4b2a76b9719d911017c592"';
    assert.strictEqual(written.headers.get('etag'), etag);
    assert.ok(written.headers.get('x-version'));
    assert.ok(Number(written.headers.get('x-modified')) >= before);
    assert.ok(written.headers.get('last-modified').endsWith(' GMT'));
    for (const method of ['HEAD', 'GET']) {
      const r = await B.fetch(url('m'), { method });
      const h = r.headers;
      assert.strictEqual(h.get('etag'), etag);
      assert.strictEqual(h.get('x-version'), written.headers.get('x-version'));
      assert.strictEqual(
        h.get('x-modified'),
        written.headers.get('x-modified')
      );
      assert.strictEqual(h.get('content-type'), 'text/x-test');
      assert.strictEqual(h.get('content-language'), 'en');
      assert.strictEqual(h.get('content-disposition'), 'attachment');
      assert.strictEqual(h.get('cache-control'), 'no-store');
      assert.strictEqual(h.get('x-meta-color'), 'blue');
      assert.strictEqual(h.get('x-meta-escaped'), '%E2%9C%93');
      if (method === 'GET') assert.strictEqual(await r.text(), 'hello');
    }
    // Metadata values are ASCII; r2.js escapes the rest.
    const accented = await put(B, 'm2', 'x', { 'x-meta-a': 'é' });
    await refusal(accented, 400, 'InvalidArgument');
  },
};

export const guards = {
  async test(ctrl, env) {
    const B = env.BUCKET;
    const first = await put(B, 'g', 'one', { 'if-none-match': '*' });
    assert.strictEqual(first.status, 200);
    const version = first.headers.get('x-version');
    await refusal(
      await put(B, 'g', 'two', { 'if-none-match': '*' }),
      412,
      'PreconditionFailed'
    );
    const second = await put(B, 'g', 'two', { 'if-match': version });
    assert.strictEqual(second.status, 200);
    // The same bytes again are a new version.
    const third = await put(B, 'g', 'two');
    assert.notStrictEqual(third.headers.get('x-version'), version);
    assert.strictEqual(third.headers.get('etag'), second.headers.get('etag'));
    await refusal(
      await put(B, 'g', 'three', {
        'if-match': second.headers.get('x-version'),
      }),
      412,
      'PreconditionFailed'
    );
    assert.strictEqual(await (await B.fetch(url('g'))).text(), 'two');
    const del = await B.fetch(url('g'), { method: 'DELETE' });
    assert.strictEqual(del.status, 204);
    const again = await B.fetch(url('g'), { method: 'DELETE' });
    assert.strictEqual(again.status, 204);
  },
};

export const largePutIsMultipart = {
  async test(ctrl, env) {
    const B = env.BUCKET;
    const size = 20 * 1024 * 1024 + 123;
    const r = await put(B, 'large', pattern(size));
    assert.strictEqual(r.status, 200);
    // Three parts of 8 MiB, 8 MiB and the rest.
    assert.match(r.headers.get('etag'), /^"[0-9a-f]{32}-3"$/);
    const got = await B.fetch(url('large'));
    assert.strictEqual(got.headers.get('content-length'), `${size}`);
    assert.strictEqual(got.headers.get('etag'), r.headers.get('etag'));
    assert.deepStrictEqual(
      await digest(got.body),
      await digest(pattern(size, 65536))
    );
    // Exactly one part is one write.
    const one = await put(B, 'one-part', pattern(8 << 20));
    assert.match(one.headers.get('etag'), /^"[0-9a-f]{32}"$/);
    // A conditional write over one part is refused: a completion takes no
    // precondition.
    const guarded = await put(B, 'guarded', pattern(9 << 20), {
      'if-none-match': '*',
    });
    await refusal(guarded, 400, 'InvalidArgument');
    const listed = await (
      await B.fetch('http://b/?list&prefix=guarded')
    ).json();
    assert.deepStrictEqual(listed.objects, []);
  },
};

export const listing = {
  async test(ctrl, env) {
    const B = env.SMALL;
    const keys = ['l/a', 'l/b/1', 'l/b/2', 'l/c', 'l/d/x/1', 'l/d/y'];
    for (const key of keys) await put(B, key, key);
    const list = async (query) => {
      const r = await B.fetch(`http://b/?list&${new URLSearchParams(query)}`);
      return r.json();
    };
    const all = await list({ prefix: 'l/' });
    assert.deepStrictEqual(
      all.objects.map((o) => o.key),
      keys
    );
    assert.strictEqual(all.token, null);
    const [b1] = all.objects.filter((o) => o.key === 'l/b/1');
    assert.strictEqual(b1.size, 5);
    assert.match(b1.etag, /^[0-9a-f]{32}$/);
    assert.ok(b1.modified > 0);

    // Pages of two, prefixes counting toward them.
    const seen = [];
    let token;
    do {
      const query = { prefix: 'l/', delimiter: '/', max: 2 };
      const page = await list(token ? { ...query, token } : query);
      seen.push([page.objects.map((o) => o.key), page.prefixes]);
      token = page.token;
    } while (token);
    assert.deepStrictEqual(seen, [
      [['l/a'], ['l/b/']],
      [['l/c'], ['l/d/']],
    ]);
    const under = await list({ prefix: 'l/d/', delimiter: '/' });
    assert.deepStrictEqual(
      [under.objects.map((o) => o.key), under.prefixes],
      [['l/d/y'], ['l/d/x/']]
    );
    const after = await list({ prefix: 'l/', 'start-after': 'l/b/1', max: 2 });
    assert.deepStrictEqual(
      after.objects.map((o) => o.key),
      ['l/b/2', 'l/c']
    );
    assert.strictEqual(after.token, 'l/c');
  },
};

export const multipart = {
  async test(ctrl, env) {
    const B = env.SMALL;
    const begun = await B.fetch(url('mp', 'uploads'), {
      method: 'POST',
      headers: { 'content-type': 'text/x-mp', 'x-meta-m': 'v' },
    });
    const { uploadId } = await begun.json();
    assert.ok(uploadId);
    const part = async (n, body) => {
      const query = `part=${n}&upload=${encodeURIComponent(uploadId)}`;
      const r = await B.fetch(url('mp', query), { method: 'PUT', body });
      return r.ok ? (await r.json()).etag : r;
    };
    const e1 = await part(1, 'abcd');
    const e2 = await part(2, 'ef');
    const listed = await B.fetch(url('mp', `upload=${uploadId}`));
    assert.deepStrictEqual(await listed.json(), [
      { part: 1, etag: e1, size: 4 },
      { part: 2, etag: e2, size: 2 },
    ]);
    const complete = (parts) =>
      B.fetch(url('mp', `upload=${uploadId}`), {
        method: 'POST',
        body: JSON.stringify(parts),
      });
    await refusal(await complete([{ part: 1, etag: e2 }]), 400, 'InvalidPart');
    await refusal(
      await complete([
        { part: 2, etag: e2 },
        { part: 1, etag: e1 },
      ]),
      400,
      'InvalidPartOrder'
    );
    const done = await complete([
      { part: 1, etag: e1 },
      { part: 2, etag: e2 },
    ]);
    assert.strictEqual(done.status, 200);
    assert.match(done.headers.get('etag'), /^"[0-9a-f]{32}-2"$/);
    assert.ok(done.headers.get('x-version'));
    const got = await B.fetch(url('mp'));
    assert.strictEqual(await got.text(), 'abcdef');
    assert.strictEqual(got.headers.get('content-type'), 'text/x-mp');
    assert.strictEqual(got.headers.get('x-meta-m'), 'v');
    assert.strictEqual(got.headers.get('etag'), done.headers.get('etag'));
    // The upload is gone.
    await refusal(await complete([{ part: 1, etag: e1 }]), 404, 'NoSuchUpload');
    await refusal(await part(1, 'x'), 404, 'NoSuchUpload');
    await refusal(
      await B.fetch(url('mp', `upload=${uploadId}`)),
      404,
      'NoSuchUpload'
    );

    // Too small a part but the last; an abort.
    const other = await (
      await B.fetch(url('mp2', 'uploads'), { method: 'POST' })
    ).json();
    const q = (n) => `part=${n}&upload=${other.uploadId}`;
    const p1 = await (
      await B.fetch(url('mp2', q(1)), { method: 'PUT', body: 'abc' })
    ).json();
    const p2 = await (
      await B.fetch(url('mp2', q(2)), { method: 'PUT', body: 'd' })
    ).json();
    const small = await B.fetch(url('mp2', `upload=${other.uploadId}`), {
      method: 'POST',
      body: JSON.stringify([
        { part: 1, etag: p1.etag },
        { part: 2, etag: p2.etag },
      ]),
    });
    await refusal(small, 400, 'EntityTooSmall');
    const aborted = await B.fetch(url('mp2', `upload=${other.uploadId}`), {
      method: 'DELETE',
    });
    assert.strictEqual(aborted.status, 204);
    const gone = await B.fetch(url('mp2', `upload=${other.uploadId}`), {
      method: 'DELETE',
    });
    await refusal(gone, 404, 'NoSuchUpload');
    await refusal(await B.fetch(url('mp2')), 404, 'NoSuchKey');
  },
};

export const keys = {
  async test(ctrl, env) {
    const B = env.SMALL;
    const odd = [
      '/a/foo.txt',
      'k//b',
      '//',
      'x/..y',
      'q?a=b&c',
      'sp ace+é',
      '%',
    ];
    for (const key of odd) {
      assert.strictEqual((await put(B, key, key)).status, 200, key);
    }
    // Plain names, unencoded, as other callers send them.
    await B.fetch('http://b/plain/name.txt', { method: 'PUT', body: 'plain' });
    for (const key of odd) {
      const r = await B.fetch(url(key));
      assert.strictEqual(await r.text(), key);
    }
    const plain = await B.fetch(url('plain/name.txt'));
    assert.strictEqual(await plain.text(), 'plain');
    const all = await (await B.fetch('http://b/?list')).json();
    for (const key of [...odd, 'plain/name.txt']) {
      assert.ok(
        all.objects.some((o) => o.key === key),
        key
      );
    }
    const slashed = await (
      await B.fetch(`http://b/?list&prefix=${encodeURIComponent('/')}`)
    ).json();
    assert.deepStrictEqual(
      slashed.objects.map((o) => o.key),
      ['//', '/a/foo.txt']
    );
    // A `.` or `..` segment could leave the binding's directory in a store
    // or proxy that resolves it.
    for (const key of ['x/../y', 'a/..', 'a/./b', '../..']) {
      for (const r of [await put(B, key, key), await B.fetch(url(key))]) {
        assert.strictEqual(r.status, 400, key);
        await r.text();
      }
    }
  },
};

// An object stored with `Content-Encoding: gzip` reads back as stored, to a
// fetch that takes it as sent (without, fetch would decode it).
export const encoded = {
  async test(ctrl, env) {
    const B = env.BUCKET;
    const text = 'hello '.repeat(100);
    const gzip = new Response(text).body.pipeThrough(
      new CompressionStream('gzip')
    );
    const stored = new Uint8Array(await new Response(gzip).arrayBuffer());
    const r = await put(B, 'gz', stored, { 'content-encoding': 'gzip' });
    assert.strictEqual(r.status, 200);
    const head = await B.fetch(url('gz'), { method: 'HEAD' });
    assert.strictEqual(head.headers.get('content-encoding'), 'gzip');
    assert.strictEqual(head.headers.get('content-length'), `${stored.length}`);
    const got = await B.fetch(url('gz'), { encodeResponseBody: 'manual' });
    const body = new Uint8Array(await got.arrayBuffer());
    assert.deepStrictEqual([body.length, body], [stored.length, stored]);
  },
};
