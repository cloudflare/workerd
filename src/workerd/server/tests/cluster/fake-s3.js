// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// The S3 calls a bucket cluster makes, for tests: GET, PUT (conditional on
// If-Match or If-None-Match: *), DELETE, and ListObjectsV2 (with a delimiter,
// common prefixes only), path-style (`/<bucket>/<key>`). One Durable Object
// holds every key, so a conditional write is atomic. Requests are not
// authenticated.

import { DurableObject } from 'cloudflare:workers';

const PAGE = 1000;
// Storage values are capped at 128 KiB: a body is stored in chunks, under keys
// outside every listed prefix.
const CHUNK = 64 * 1024;
const chunkKey = (key, i) => `\0${key}\0${i}`;

function escape(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

export class Bucket extends DurableObject {
  next = 0;
  // Requests per object key, for tests that bound a cluster's bucket traffic.
  requests = new Map();

  async fetch(request) {
    const storage = this.ctx.storage;
    const url = new URL(request.url);
    if (url.pathname === '/_requests') {
      return Response.json(this.requests.get(url.searchParams.get('key')) ?? 0);
    }
    const path = decodeURIComponent(url.pathname).slice(1);
    const slash = path.indexOf('/');
    const key = slash < 0 ? '' : path.slice(slash + 1);
    const headers = { date: new Date().toUTCString() };

    if (request.method === 'GET' && key === '') {
      const prefix = url.searchParams.get('prefix') ?? '';
      // With a delimiter, one unpaged answer: tests' buckets are small.
      const delimiter = url.searchParams.get('delimiter');
      if (delimiter !== null) {
        const common = new Set();
        for (const k of (await storage.list({ prefix })).keys()) {
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
      const after = url.searchParams.get('continuation-token');
      const listed = await storage.list({
        prefix,
        ...(after === null ? {} : { startAfter: after }),
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

    // Read the body before the object: storage calls keep other requests
    // out, but awaiting the body would let one in between the check and the
    // write.
    const counted = key.slice(key.indexOf('/') + 1);
    this.requests.set(counted, (this.requests.get(counted) ?? 0) + 1);
    const body = request.method === 'PUT' ? await request.arrayBuffer() : null;
    const object = await storage.get(key);
    switch (request.method) {
      case 'GET':
        if (!object) return new Response(null, { status: 404, headers });
        const keys = Array.from({ length: object.chunks }, (_, i) =>
          chunkKey(key, i)
        );
        const chunks = keys.length === 0 ? new Map() : await storage.get(keys);
        const out = new Uint8Array(object.size);
        keys.forEach((k, i) =>
          out.set(new Uint8Array(chunks.get(k)), i * CHUNK)
        );
        return new Response(out, {
          headers: { ...headers, etag: object.etag },
        });
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
        const entries = {};
        for (let i = 0; i < chunks; i++)
          entries[chunkKey(key, i)] = body.slice(i * CHUNK, (i + 1) * CHUNK);
        entries[key] = { etag, size: body.byteLength, chunks };
        await storage.put(entries);
        return new Response(null, { headers: { ...headers, etag } });
      }
      case 'DELETE':
        await storage.delete(key);
        return new Response(null, { status: 204, headers });
    }
    return new Response(null, { status: 405, headers });
  }
}

export default {
  fetch(request, env) {
    return env.BUCKET.get(env.BUCKET.idFromName('bucket')).fetch(request);
  },
};
