// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

// Every page of a listing under `prefix`, with its objects' custom metadata.
async function pages(bucket, options) {
  const out = [];
  let cursor;
  do {
    const page = await bucket.list({
      ...options,
      include: ['customMetadata'],
      cursor,
    });
    out.push(page);
    cursor = page.truncated ? page.cursor : undefined;
  } while (cursor);
  return out;
}

// A listing whose reply would pass r2.js's budget of 512 KiB (the binding
// refuses over 1 MiB) stops short, with a cursor that resumes after its
// last key. Each object's metadata is near the 8 KiB limit, so a few dozen
// pass the budget; every object is a round trip, slow in debug builds.
export const largeListings = {
  async test(ctrl, env) {
    const customMetadata = { m: 'x'.repeat(8000) };
    const keys = Array.from(
      { length: 70 },
      (_, i) => `l/${String(i).padStart(3, '0')}`
    );
    await Promise.all(
      keys.map((k) => env.BUCKET.put(k, '', { customMetadata }))
    );
    const listed = await pages(env.BUCKET, { prefix: 'l/' });
    assert.ok(listed.length > 1, `${listed.length} page`);
    assert.deepStrictEqual(
      listed.flatMap((p) => p.objects.map((o) => o.key)),
      keys.toSorted()
    );
    await env.BUCKET.delete(keys);
  },
};

// The budget is bytes, not characters, and counts prefixes, which a page
// may stop at: here each object is 12 KB of UTF-8 in 4,000 characters, and
// is followed by a prefix of almost 1 KB. The listing passes the budget
// only in bytes, so it takes more than one page only if bytes are counted.
export const listingBudgetInBytes = {
  async test(ctrl, env) {
    const wide = { customMetadata: { m: '字'.repeat(4000) } };
    const names = Array.from(
      { length: 50 },
      (_, i) => `w/${String(i).padStart(3, '0')}`
    );
    const long = (n) => `${n}${'x'.repeat(900)}/`;
    await Promise.all(
      names.flatMap((n) => [
        env.BUCKET.put(n, '', wide),
        env.BUCKET.put(`${long(n)}k`, ''),
      ])
    );
    const seen = await pages(env.BUCKET, { prefix: 'w/', delimiter: '/' });
    assert.ok(seen.length > 1, `${seen.length} page`);
    assert.deepStrictEqual(
      seen.flatMap((p) => p.objects.map((o) => o.key)),
      names
    );
    assert.deepStrictEqual(
      seen.flatMap((p) => p.delimitedPrefixes),
      names.map(long)
    );
    await env.BUCKET.delete([...names, ...names.map((n) => `${long(n)}k`)]);
  },
};
