// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { Buffer } from 'node:buffer';
import { createHash } from 'node:crypto';

// Miniflare's R2 tests, in workers-sdk, run the binding against r2.js;
// these cover what they don't. Sizes are in units of 16 KiB.
const CHUNK = 16 * 1024;

// `size` bytes, each its index mod 251, so any range is checkable.
function bytes(size, from = 0) {
  const out = new Uint8Array(size);
  for (let i = 0; i < size; i++) out[i] = (from + i) % 251;
  return out;
}

const same = (a, b) =>
  assert.ok(Buffer.from(a).equals(Buffer.from(b)), 'bytes differ');

// `o` without its undefined fields, which some compatibility flags add.
const defined = (o) =>
  Object.fromEntries(Object.entries(o).filter(([, v]) => v !== undefined));

const md5 = (b) => createHash('md5').update(b).digest('hex');

// Buckets apart, keys that need escaping, metadata that isn't ASCII (as a
// content header too), one upload time, R2's storage class and
// `If-None-Match: *`.
export const bucketsKeysAndMetadata = {
  async test(ctrl, env) {
    const contentDisposition = 'attachment; filename="résumé.pdf"';
    const put = await env.BUCKET.put('k', 'hello', {
      customMetadata: { a: '1', é: 'ü' },
      httpMetadata: { contentDisposition },
    });
    assert.strictEqual(put.storageClass, 'Standard');
    const got = await env.BUCKET.get('k');
    assert.deepStrictEqual(got.customMetadata, { a: '1', é: 'ü' });
    assert.strictEqual(got.httpMetadata.contentDisposition, contentDisposition);
    // One time, however the object is read.
    assert.strictEqual(got.uploaded.getTime(), put.uploaded.getTime());
    assert.strictEqual(await (await env.SAME.get('k')).text(), 'hello');
    // A listing names the version a head does.
    const listed = (await env.BUCKET.list({ prefix: 'k' })).objects[0];
    assert.strictEqual(listed.version, put.version);
    assert.strictEqual(await env.OTHER.get('k'), null);
    const odd = 'a/b c?d=e&f%20é/..x';
    await env.BUCKET.put(odd, 'odd');
    assert.strictEqual(await (await env.BUCKET.get(odd)).text(), 'odd');
    const star = new Headers({ 'if-none-match': '*' });
    assert.strictEqual(await env.BUCKET.put(odd, 'x', { onlyIf: star }), null);
    await assert.rejects(env.BUCKET.put('a/../b', 'x'), /\(10020\)/);
    await env.BUCKET.delete([odd, 'k']);
    assert.strictEqual(await env.BUCKET.head(odd), null);
  },
};

// Values inline, in one chunk, and in many, written as streams and read
// back, whole and in ranges that cross chunk boundaries.
export const largeAndRanges = {
  async test(ctrl, env) {
    for (const size of [1000, 70_000, 10 * CHUNK + 123]) {
      const value = bytes(size);
      let sent = 0;
      const { readable, writable } = new FixedLengthStream(size);
      const w = writable.getWriter();
      const pump = (async () => {
        while (sent < size) await w.write(value.slice(sent, (sent += 5000)));
        await w.close();
      })();
      const put = await env.BUCKET.put(`v${size}`, readable);
      await pump;
      assert.strictEqual(put.etag, md5(value));
      const got = await env.BUCKET.get(`v${size}`);
      same(await got.arrayBuffer(), value);
      const cases = [
        [{ offset: 0, length: 1 }, 0, 1],
        [{ offset: size - 10 }, size - 10, 10],
        [{ suffix: 100 }, size - 100, 100],
        [{ offset: CHUNK - 3, length: 2 * CHUNK }, CHUNK - 3, 2 * CHUNK],
        [{ length: size + 100 }, 0, size],
      ];
      // A length a Number can't hold exactly reads to the end, not past
      // u64::MAX.
      const huge = { offset: 1, length: '18446744073709551614' };
      const request = { method: 'get', object: `v${size}`, range: huge };
      const raw = await env.R2_WORKER.fetch('http://r2/', {
        headers: { 'cf-r2-request': JSON.stringify(request) },
      });
      assert.strictEqual(raw.status, 200, raw.headers.get('cf-r2-error'));
      await raw.body.cancel();
      for (const [range, offset, n] of cases) {
        if (offset + n > size) continue;
        const r = await env.BUCKET.get(`v${size}`, { range });
        assert.deepStrictEqual(defined(r.range), { offset, length: n });
        same(await r.arrayBuffer(), bytes(n, offset));
      }
      const h = new Headers({ range: `bytes=${size - 300}-${size - 1}` });
      const r = await env.BUCKET.get(`v${size}`, { range: h });
      same(await r.arrayBuffer(), bytes(300, size - 300));
    }
    await assert.rejects(
      env.BUCKET.get('v1000', { range: { offset: 2000 } }),
      /\(10039\)/
    );
  },
};

// Checksums checked as a value streams, across chunks and parts.
export const checksums = {
  async test(ctrl, env) {
    const value = 'checked';
    await env.BUCKET.put('sum', value);
    await assert.rejects(
      env.BUCKET.put('sum', 'x'.repeat(70_000), { md5: '0'.repeat(32) }),
      /\(10037\)/
    );
    // A failed put leaves the object as it was, a multipart one too.
    const big = bytes(9 * 1024 * 1024 + 1);
    await assert.rejects(
      env.BUCKET.put('sum', big, { sha1: '0'.repeat(40) }),
      /\(10037\)/
    );
    assert.strictEqual(await (await env.BUCKET.get('sum')).text(), value);
    // A multipart one that passes keeps its SHA checksums.
    const sha1 = createHash('sha1').update(big).digest('hex');
    await env.BUCKET.put('big', big, { sha1 });
    const kept = await env.BUCKET.head('big');
    assert.match(kept.etag, /-2$/);
    assert.strictEqual(Buffer.from(kept.checksums.sha1).toString('hex'), sha1);
  },
};

// The Worker refuses requests it cannot serve.
export const badRequests = {
  async test(ctrl, env) {
    const r = await env.R2_WORKER.fetch('http://r2/');
    assert.strictEqual(r.status, 400);
    assert.match(r.headers.get('cf-r2-error'), /10029/);
    const negative = await env.R2_WORKER.fetch('http://r2/', {
      method: 'PUT',
      headers: { 'cf-r2-metadata-size': '-1' },
    });
    assert.strictEqual(negative.status, 400);
    assert.match(negative.headers.get('cf-r2-error'), /10029/);
    const garbled = await env.R2_WORKER.fetch('http://r2/', {
      method: 'PUT',
      headers: { 'cf-r2-metadata-size': '2' },
      body: new Uint8Array([0x7b, 0xff]),
    });
    assert.strictEqual(garbled.status, 400);
    assert.match(garbled.headers.get('cf-r2-error'), /10029/);
    // A put needs its length.
    const meta = JSON.stringify({ method: 'put', object: 'k' });
    const unsized = await env.R2_WORKER.fetch('http://r2/', {
      method: 'PUT',
      headers: { 'cf-r2-metadata-size': String(meta.length) },
      body: new ReadableStream({
        start(c) {
          c.enqueue(new TextEncoder().encode(meta));
          c.close();
        },
      }),
    });
    assert.strictEqual(unsized.status, 400);
    assert.match(unsized.headers.get('cf-r2-error'), /10029/);
    // Anything else is R2's InternalError, not a bare 500.
    const bucket = JSON.stringify({ method: 'createBucket', bucket: 'b' });
    const unknown = await env.R2_WORKER.fetch('http://r2/', {
      method: 'PUT',
      headers: { 'cf-r2-metadata-size': String(bucket.length) },
      body: bucket,
    });
    assert.strictEqual(unknown.status, 500);
    assert.match(unknown.headers.get('cf-r2-error'), /10001/);
  },
};

// A get's body, whole or a range, put as another object (Miniflare's
// "put: can copy values"), inline and across chunks.
export const copyValues = {
  async test(ctrl, env) {
    for (const size of [10, 70_000]) {
      await env.BUCKET.put('src', bytes(size));
      const copy = async (options) => {
        const object = await env.BUCKET.get('src', options);
        await env.BUCKET.put('dst', object.body);
        return new Uint8Array(
          await (await env.BUCKET.get('dst')).arrayBuffer()
        );
      };
      same(await copy(), bytes(size));
      same(await copy({ range: { offset: 1, length: 4 } }), bytes(4, 1));
      same(await copy({ range: { suffix: 5 } }), bytes(5, size - 5));
      const range = new Headers({ range: 'bytes=0-5' });
      same(await copy({ range }), bytes(6));
    }
  },
};

// A reply has its objects and prefixes in UTF-8 byte order, as stores sort
// keys, which UTF-16's is not: U+FF61 is before U+1F600 in UTF-8, after
// its surrogates in UTF-16.
export const listingOrder = {
  async test(ctrl, env) {
    const names = ['o/a', 'o/\uFF61', 'o/\u{1F600}'];
    const keys = names.flatMap((n) => [n, `${n}/k`]);
    await Promise.all(keys.map((k) => env.BUCKET.put(k, '')));
    const page = await env.BUCKET.list({ prefix: 'o/', delimiter: '/' });
    assert.strictEqual(page.truncated, false);
    assert.deepStrictEqual(
      page.objects.map((o) => o.key),
      names
    );
    assert.deepStrictEqual(
      page.delimitedPrefixes,
      names.map((n) => `${n}/`)
    );
    await env.BUCKET.delete(keys);
  },
};

// A listing whose reply would pass the binding's 1 MiB stops short, with a
// cursor that resumes after its last key.
export const largeListings = {
  async test(ctrl, env) {
    const customMetadata = { m: 'x'.repeat(6000) };
    const keys = Array.from(
      { length: 100 },
      (_, i) => `l/${String(i).padStart(3, '0')}`
    );
    await Promise.all(
      keys.map((k) => env.BUCKET.put(k, '', { customMetadata }))
    );
    const listed = [];
    let cursor;
    let pages = 0;
    do {
      const page = await env.BUCKET.list({
        prefix: 'l/',
        include: ['customMetadata'],
        cursor,
      });
      listed.push(...page.objects.map((o) => o.key));
      cursor = page.truncated ? page.cursor : undefined;
      pages++;
    } while (cursor);
    assert.ok(pages > 1, `${pages} page`);
    assert.deepStrictEqual(listed, keys.toSorted());
    // A page HEADs its objects only as far as its budget reaches, and
    // never the last here, whose HEAD would fail.
    await env.PROBE.fetch('http://control/', {
      method: 'POST',
      body: keys.at(-1),
    });
    const cut = await env.PROBED.list({
      prefix: 'l/',
      include: ['customMetadata'],
    });
    assert.ok(cut.truncated);
    const { heads } = await (await env.PROBE.fetch('http://control/')).json();
    assert.ok(heads < keys.length, `${heads} HEADs`);
    await env.BUCKET.delete(keys);

    // The budget is bytes, not characters, and counts prefixes, which a
    // page may stop at: here each object is over 10 KB of UTF-8 in 3,500
    // characters, and is followed by a prefix of almost 1 KB.
    const wide = { customMetadata: { m: '字'.repeat(3500) } };
    const names = keys.map((k) => `w/${k.slice(2)}`);
    const long = (n) => `${n}${'x'.repeat(900)}/`;
    await Promise.all(
      names.flatMap((n) => [
        env.BUCKET.put(n, '', wide),
        env.BUCKET.put(`${long(n)}k`, ''),
      ])
    );
    const seen = { objects: [], prefixes: [] };
    cursor = undefined;
    do {
      const page = await env.BUCKET.list({
        prefix: 'w/',
        delimiter: '/',
        include: ['customMetadata'],
        cursor,
      });
      seen.objects.push(...page.objects.map((o) => o.key));
      seen.prefixes.push(...page.delimitedPrefixes);
      cursor = page.truncated ? page.cursor : undefined;
    } while (cursor);
    assert.deepStrictEqual(seen.objects, names);
    assert.deepStrictEqual(seen.prefixes, names.map(long));
    await env.BUCKET.delete([...names, ...names.map((n) => `${long(n)}k`)]);
  },
};
