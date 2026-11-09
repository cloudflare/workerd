// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// A stand-in for S3, for trying the sample: the calls a workerd cluster makes
// on its bucket. GET, PUT (conditional on If-Match or If-None-Match: *),
// DELETE, and ListObjectsV2 (with a delimiter, common prefixes only),
// path-style (`/<bucket>/<key>`). One Durable Object holds every key, so a
// conditional write is atomic. Requests are not authenticated. Not for
// production use.

import { DurableObject } from 'cloudflare:workers';

const PAGE = 1000;
// Storage values are capped at 128 KiB: a body is stored in chunks, under keys
// that start with NUL. Listings start at FIRST, after them.
const CHUNK = 64 * 1024;
const chunkKey = (key, i) => `\0${key}\0${i}`;
const FIRST = '\x01';
const chunkKeys = (key, object) =>
  Array.from({ length: object?.chunks ?? 0 }, (_, i) => chunkKey(key, i));

function escape(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

export class Bucket extends DurableObject {
  next = 0;

  async fetch(request) {
    const storage = this.ctx.storage;
    const url = new URL(request.url);
    const path = decodeURIComponent(url.pathname).slice(1);
    const slash = path.indexOf('/');
    const key = slash < 0 ? '' : path.slice(slash + 1);
    const headers = { date: new Date().toUTCString() };

    if (request.method === 'GET' && key === '') {
      return this.list(url.searchParams, headers);
    }

    // Read the body before the object: storage calls keep other requests
    // out, but awaiting the body would let one in between the check and the
    // write.
    const body = request.method === 'PUT' ? await request.arrayBuffer() : null;
    const object = await storage.get(key);
    switch (request.method) {
      case 'GET': {
        if (!object) return new Response(null, { status: 404, headers });
        const keys = chunkKeys(key, object);
        const chunks = keys.length === 0 ? new Map() : await storage.get(keys);
        const out = new Uint8Array(object.size);
        keys.forEach((k, i) =>
          out.set(new Uint8Array(chunks.get(k)), i * CHUNK)
        );
        return new Response(out, {
          headers: { ...headers, etag: object.etag },
        });
      }
      case 'PUT': {
        const ifMatch = request.headers.get('if-match');
        const ifNoneMatch = request.headers.get('if-none-match');
        if (
          (ifNoneMatch === '*' && object) ||
          (ifMatch !== null && object?.etag !== ifMatch)
        ) {
          return new Response(null, { status: 412, headers });
        }
        const etag = `"${Date.now()}-${this.next++}"`;
        const chunks = Math.ceil(body.byteLength / CHUNK);
        const stale = chunkKeys(key, object).slice(chunks);
        if (stale.length > 0) await storage.delete(stale);
        const entries = {};
        for (let i = 0; i < chunks; i++)
          entries[chunkKey(key, i)] = body.slice(i * CHUNK, (i + 1) * CHUNK);
        entries[key] = { etag, size: body.byteLength, chunks };
        await storage.put(entries);
        return new Response(null, { headers: { ...headers, etag } });
      }
      case 'DELETE':
        await storage.delete([key, ...chunkKeys(key, object)]);
        return new Response(null, { status: 204, headers });
    }
    return new Response(null, { status: 405, headers });
  }

  async list(params, headers) {
    const storage = this.ctx.storage;
    const prefix = params.get('prefix') ?? '';
    // With a delimiter, one unpaged answer.
    const delimiter = params.get('delimiter');
    if (delimiter !== null) {
      const common = new Set();
      for (const k of (await storage.list({ prefix, start: FIRST })).keys()) {
        const end = k.indexOf(delimiter, prefix.length);
        if (end >= 0) common.add(k.slice(0, end + delimiter.length));
      }
      const xml =
        '<ListBucketResult><IsTruncated>false</IsTruncated>' +
        [...common]
          .map(
            (p) =>
              `<CommonPrefixes><Prefix>${escape(p)}</Prefix></CommonPrefixes>`
          )
          .join('') +
        '</ListBucketResult>';
      return new Response(xml, { headers });
    }
    const after = params.get('continuation-token');
    const listed = await storage.list({
      prefix,
      ...(after === null ? { start: FIRST } : { startAfter: after }),
      limit: PAGE + 1,
    });
    const page = [...listed].slice(0, PAGE);
    const truncated = listed.size > PAGE;
    const xml =
      '<ListBucketResult>' +
      `<IsTruncated>${truncated}</IsTruncated>` +
      page
        .map(
          ([k, v]) =>
            `<Contents><Key>${escape(k)}</Key><Size>${v.size}</Size></Contents>`
        )
        .join('') +
      (truncated
        ? `<NextContinuationToken>${escape(page.at(-1)[0])}</NextContinuationToken>`
        : '') +
      '</ListBucketResult>';
    return new Response(xml, { headers });
  }
}

export default {
  fetch(request, env) {
    return env.BUCKET.get(env.BUCKET.idFromName('bucket')).fetch(request);
  },
};
