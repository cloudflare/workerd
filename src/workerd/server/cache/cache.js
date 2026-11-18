// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The Cache API, on the protocol workerd's `caches` speak to a Worker's
// `cacheApiOutbound` (src/workerd/api/cache.c++):
//
//   GET <url>    match, with `Cache-Control: only-if-cached`: a hit is any
//                response with `CF-Cache-Status: HIT`, a miss a 504 with
//                `CF-Cache-Status: MISS`.
//   PUT <url>    put: the body is the response to store, serialized as
//                HTTP/1.1. 204 stored, 413 not.
//   PURGE <url>  delete: 200 deleted, 404 not found.
//
// `caches.open(name)` sends `CF-Cache-Namespace: <name, URI-encoded>`; the
// default cache sends none, and is disjoint from every named one. A
// request's `cf.cacheKey`, when set, is its key instead of its URL.
//
// The config that runs this module (`bundled = "/workerd/cache.js"`) is an
// interface: cli/wrangler.rs, Miniflare and hand-written configs write it,
// each for the workerd it ships with or runs on. Anything added to it later
// must be optional, so that configs written before keep working. It is:
//
//   - a Worker that Workers name as their `cacheApiOutbound`, with a
//     compatibility date of 2022-11-30 or later, or the flags
//     `streams_enable_constructors` and
//     `transformstream_enable_standard_constructor`;
//   - these bindings:
//
//   STORE       where entries live: a writable `disk`, or in a cluster a
//               service that speaks the same protocol
//   TEST_CLOCK  JSON `true` for tests (optional)
//
// An entry is one record in `STORE` at `/<key>`, the lowercase hex SHA-256
// of `<CF-Cache-Namespace as sent, or empty>\0<cf.cacheKey or URL>`:
//
//   GET     200 with the record and its Content-Length, or 404
//   PUT     the record, with `X-Expires: <ms since the epoch>`, after which
//           the store may drop it: 2xx stored, 413 refused. A body that
//           fails must not be stored, but a `disk` sees one without a
//           length that fails as one that ends: this Worker then cancels
//           the request first.
//   DELETE  2xx had an unexpired record, 404 did not. A `disk` does not
//           know expiry: there an entry that expired unread is deleted.
//
// A record is a 4-byte big-endian length, that many bytes of JSON
// `{status, statusText, headers: [[name, value], ...], expires}`, then the
// body as it was put, still content-encoded. Bodies stream through this
// Worker; only a match of several ranges buffers one.
//
// Whether a response is stored, and for how long, follows Miniflare, which
// asks http-cache-semantics as a shared cache. Not stored: `no-store`,
// `private`, `Set-Cookie` (`private=set-cookie` drops the cookie instead),
// an `Authorization` request header without `public`, `s-maxage` or
// `must-revalidate`, a status it does not know, or over 512 MiB. Its
// lifetime is `s-maxage`, else `max-age`, else `Expires` minus `Date`,
// else a tenth of the time since `Last-Modified`, less `Age`; `immutable`
// makes it at least a day. A lifetime of 0, as with `no-cache` or no
// freshness information, stores nothing and deletes what was there. An
// expired entry, or one that is not a record, is a miss, and is deleted
// when read. A put's head is at most 1 MiB.
//
// With `TEST_CLOCK`, for tests, POST /clock?advance=<ms> moves this
// process's clock.

const MAX_ENTRY = 512 * 1024 * 1024;
const MAX_HEAD = 1024 * 1024;
// The statuses http-cache-semantics stores, and those it stores without
// an explicit lifetime.
const KNOWN = [
  200, 203, 204, 300, 301, 302, 303, 307, 308, 404, 405, 410, 414, 501,
];
const BY_DEFAULT = [200, 203, 204, 300, 301, 308, 404, 405, 410, 414, 501];
const TOKEN = /^[!#$%&'*+.^_`|~\w-]+$/;
const HOP_BY_HOP = [
  'connection',
  'keep-alive',
  'proxy-authenticate',
  'proxy-authorization',
  'te',
  'trailer',
  'transfer-encoding',
  'upgrade',
];
const NULL_BODY = [101, 204, 205, 304];
const enc = new TextEncoder();
const dec = new TextDecoder();

class Fail extends Error {
  constructor(status, text, headers) {
    super(text);
    this.status = status;
    this.headers = headers;
  }
}

let skew = 0;
const clock = () => Date.now() + skew;

const failed = (e) => {
  const statusText = String(e.message ?? e).replace(/[\r\n]/g, ' ');
  const { status = 500, headers } = e;
  return new Response(null, { status, statusText, headers });
};

export default {
  async fetch(request, env) {
    return serve(request, env).catch(failed);
  },
};

export const ReadThrough = {
  async fetch(request, env, ctx) {
    return readThrough(request, env, ctx).catch(failed);
  },
};

async function serve(request, env) {
  if (request.method === 'POST' && new URL(request.url).pathname === '/purge') {
    return purge(request, env);
  }
  if (request.method === 'POST' && env.TEST_CLOCK) {
    const url = new URL(request.url);
    const advance = Number(url.searchParams.get('advance'));
    if (url.pathname !== '/clock' || !Number.isFinite(advance)) {
      throw new Fail(400, 'Expected POST /clock?advance=<ms>');
    }
    skew += advance;
    return new Response(null);
  }
  const ns = request.headers.get('CF-Cache-Namespace') ?? '';
  const key = request.cf?.cacheKey ? String(request.cf.cacheKey) : request.url;
  const at = await storeUrl(`${ns}\0${key}`);
  switch (request.method) {
    case 'GET':
      return match(request, env, at);
    case 'PUT':
      return put(request, env, at);
    case 'PURGE':
      // Not read first: in a cluster, a read leases a copy, which the delete
      // then waits out.
      return remove(env, at);
  }
  throw new Fail(405, 'Method Not Allowed');
}

const hex = (bytes) =>
  Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

const storeUrl = async (name) => {
  const digest = await crypto.subtle.digest('SHA-256', enc.encode(name));
  return `http://store/${hex(new Uint8Array(digest))}`;
};

const concat = (a, b) => {
  const out = new Uint8Array(a.byteLength + b.byteLength);
  out.set(a);
  out.set(b, a.byteLength);
  return out;
};

// `first`, then the rest of `reader`, as a stream. `check(n, error)` sees
// the running total after each chunk, or the reader's error, and throws to
// fail the stream.
function rest(first, reader, check = () => {}) {
  let n = 0;
  return new ReadableStream({
    start(c) {
      check((n += first.byteLength));
      if (first.byteLength > 0) c.enqueue(first);
    },
    async pull(c) {
      const { value, done } = await reader.read().catch((e) => {
        throw (check(n, e), e);
      });
      if (done) return c.close();
      check((n += value.byteLength));
      c.enqueue(value);
    },
    cancel: (reason) => reader.cancel(reason),
  });
}

// `{head, body, size}` of the unexpired entry at `at`, or null, after
// which `miss.ticket` is the store's for a fill. One that expired, or is
// not a record, is deleted.
async function load(env, at, miss = {}) {
  const r = await env.STORE.fetch(at);
  if (r.status === 404) {
    miss.ticket = r.headers.get('X-Ticket');
    return null;
  }
  if (!r.ok) throw new Error(`Reading the entry failed: ${r.status}.`);
  const reader = r.body.getReader({ mode: 'byob' });
  // `n` bytes, or null at the end.
  const read = async (n) => {
    const { value } = await reader.readAtLeast(n, new Uint8Array(n));
    return value?.byteLength === n ? value : null;
  };
  const prefix = await read(4);
  const n = prefix && new DataView(prefix.buffer).getUint32(0);
  const json = n > 0 && n <= MAX_HEAD ? await read(n) : null;
  let head = null;
  try {
    head = JSON.parse(dec.decode(json ?? new Uint8Array(0)));
    new Headers(head.headers);
  } catch {
    head = null;
  }
  if (head?.expires > clock()) {
    reader.releaseLock();
    const size = Number(r.headers.get('Content-Length')) - 4 - n;
    // workerd pumps it: a stream read from cannot be a body itself.
    const body = r.body.pipeThrough(new IdentityTransformStream());
    return { head, body, size };
  }
  await reader.cancel().catch(() => {});
  await remove(env, at).catch(() => {});
  return null;
}

async function remove(env, at) {
  const r = await env.STORE.fetch(at, { method: 'DELETE' });
  if (r.status === 404) throw new Fail(404, 'Not Found');
  if (!r.ok) throw new Error(`Deleting the entry failed: ${r.status}.`);
  return new Response(null);
}

// =======================================================================
// Match

async function match(request, env, at) {
  const entry = await load(env, at);
  if (entry === null) {
    throw new Fail(504, 'Gateway Timeout', { 'CF-Cache-Status': 'MISS' });
  }
  return answer(request, entry, 'HIT');
}

// `entry` as `request` asks: whole, by ranges, or a 304. `cacheStatus` is
// its CF-Cache-Status.
async function answer(request, entry, cacheStatus) {
  const { head, size } = entry;
  let body = entry.body;
  const headers = new Headers(head.headers);
  headers.set('CF-Cache-Status', cacheStatus);
  const range = request.headers.get('Range');
  const ranges = range === null ? [] : parseRanges(range, size);
  headers.delete('Content-Length');
  if (ranges === undefined) {
    await body.cancel();
    headers.set('Content-Range', `bytes */${size}`);
    throw new Fail(416, 'Range Not Satisfiable', headers);
  }
  if (notModified(request.headers, headers)) {
    await body.cancel();
    return new Response(null, { status: 304, headers });
  }
  let { status, statusText } = head;
  if (ranges.length > 0) [status, statusText] = [206, 'Partial Content'];
  if (ranges.length === 0) {
    headers.set('Content-Length', `${size}`);
  } else if (ranges.length === 1) {
    const { start, end } = ranges[0];
    headers.set('Content-Range', `bytes ${start}-${end}/${size}`);
    headers.set('Content-Length', `${end - start + 1}`);
    body = slice(body, start, end);
  } else if (ranges.length > 1) {
    const bytes = new Uint8Array(await new Response(body).arrayBuffer());
    const type = headers.get('Content-Type');
    const boundary = `workerd-boundary-${crypto.randomUUID()}`;
    const parts = ranges.flatMap(({ start, end }) => [
      `--${boundary}\r\n`,
      type === null ? '' : `Content-Type: ${type}\r\n`,
      `Content-Range: bytes ${start}-${end}/${size}\r\n\r\n`,
      bytes.subarray(start, end + 1),
      '\r\n',
    ]);
    body = await new Blob([...parts, `--${boundary}--`]).arrayBuffer();
    headers.set('Content-Type', `multipart/byteranges; boundary=${boundary}`);
    headers.set('Content-Length', `${body.byteLength}`);
  }
  if (NULL_BODY.includes(status)) {
    await body.cancel();
    body = null;
  }
  return new Response(body, {
    status,
    statusText,
    headers,
    encodeBody: 'manual',
  });
}

// Bytes `start` to `end`, inclusive, of `body`.
function slice(body, start, end) {
  let at = 0;
  return body.pipeThrough(
    new TransformStream({
      transform(chunk, c) {
        const from = Math.max(start - at, 0);
        const to = Math.min(end + 1 - at, chunk.byteLength);
        at += chunk.byteLength;
        if (from < to) c.enqueue(chunk.subarray(from, to));
      },
    })
  );
}

// Whether `If-None-Match` (weakly) or `If-Modified-Since` makes this a 304.
function notModified(req, res) {
  const etag = parseETag(res.get('ETag') ?? '');
  const inm = req.get('If-None-Match');
  if (inm !== null && etag !== undefined) {
    if (inm.trim() === '*') return true;
    if (inm.split(',').some((t) => parseETag(t) === etag)) return true;
  }
  // NaN, for a missing or malformed date, compares false.
  return (
    parseDate(res.get('Last-Modified')) <=
    parseDate(req.get('If-Modified-Since'))
  );
}

const parseETag = (value) => /^(W\/)?"(.+)"$/.exec(value.trim())?.[2];

const DATE = new RegExp(
  '^(Mon|Tue|Wed|Thu|Fri|Sat|Sun), \\d\\d (Jan|Feb|Mar|Apr|May|Jun|Jul|' +
    'Aug|Sep|Oct|Nov|Dec) \\d{4} \\d\\d:\\d\\d:\\d\\d GMT$'
);
const parseDate = (v) => (v !== null && DATE.test(v) ? Date.parse(v) : NaN);

// A Range header's inclusive ranges of `size` bytes, as Miniflare's
// shared/range.ts parses it: [] for the whole body, undefined if
// unsatisfiable.
function parseRanges(header, size) {
  const prefix = /^ *bytes *=/i.exec(header);
  if (prefix === null) return undefined;
  const spec = header.slice(prefix[0].length);
  if (spec.trimStart() === '') return [];
  const out = [];
  for (const part of spec.split(',')) {
    const m = /^ *(\d+)? *- *(\d+)? *$/.exec(part);
    if (m === null) return undefined;
    const [start, end] = [m[1], m[2]].map((v) => v && parseInt(v));
    if (start !== undefined) {
      if (start >= size || start > (end ?? start)) return undefined;
      out.push({ start, end: Math.min(end ?? size, size - 1) });
    } else if (end === undefined) {
      return undefined;
    } else if (end >= size) {
      return [];
    } else if (end > 0) {
      out.push({ start: size - end, end: size - 1 });
    }
  }
  return out;
}

// =======================================================================
// Put

async function put(request, env, at) {
  const reader = request.body.getReader();
  const drain = async () => {
    while (!(await reader.read()).done);
  };
  const refuse = async (status, text) => {
    await drain();
    throw new Fail(status, text);
  };
  let buf = new Uint8Array(0);
  let end;
  while ((end = blankLine(buf)) < 0) {
    if (buf.byteLength > MAX_HEAD) await refuse(400, 'Bad Request');
    const { value, done } = await reader.read();
    if (done) throw new Fail(400, 'Bad Request');
    buf = concat(buf, value);
  }
  const [line, ...lines] = dec.decode(buf.subarray(0, end)).split('\r\n');
  const m = /^HTTP\/\d(?:\.\d)? (\d{3})(?: (.*))?$/.exec(line);
  const parsed = lines.map((h) => {
    const i = h.indexOf(':');
    return [i < 0 ? '' : h.slice(0, i), h.slice(i + 1).trim()];
  });
  if (m === null || parsed.some(([k]) => !TOKEN.test(k))) {
    await refuse(400, 'Bad Request');
  }
  const status = parseInt(m[1]);
  const policy = lifetime(status, parsed, request.headers);
  const length = Number(
    parsed.find(([k]) => k.toLowerCase() === 'content-length')?.[1] ?? NaN
  );
  if (policy === null || length > MAX_ENTRY) {
    await refuse(413, 'Payload Too Large');
  }
  // A body with a length, in a request with one, must match it.
  const outer = request.headers.get('Content-Length');
  const known = length >= 0 && outer !== null;
  if (known && Number(outer) !== end + 4 + length) {
    await refuse(400, 'Bad Request');
  }
  const { ms, headers } = policy;
  if (ms <= 0) {
    await drain();
    await remove(env, at).catch((e) => {
      if (e.status !== 404) throw e;
    });
    return new Response(null, { status: 204 });
  }
  const expires = Math.min(Math.floor(clock() + ms), Number.MAX_SAFE_INTEGER);
  const json = enc.encode(
    JSON.stringify({ status, statusText: m[2] ?? '', headers, expires })
  );
  if (json.byteLength > MAX_HEAD) await refuse(400, 'Bad Request');
  const prefix = new Uint8Array(4);
  new DataView(prefix.buffer).setUint32(0, json.byteLength);
  const skip = 4 + json.byteLength;
  // A failed body may still reach the store as a clean end: in workerd, a
  // service binding's request body ends rather than fails when its stream
  // errors, unless it has a length. So a put with a length sends one, and
  // the store keeps what it had; one without is cancelled if it fails, and
  // deleted should the store have taken it. When both lengths are known
  // workerd pumps the body; else it streams through here, counted.
  let failed = null;
  let pumped;
  let body;
  // Cancels the store's request before it sees the failed body end.
  const abort = new AbortController();
  const fail = (e) => abort.abort((failed = e));
  const first = concat(concat(prefix, json), buf.subarray(end + 4));
  if (known) {
    const fixed = new FixedLengthStream(skip + length);
    const writer = fixed.writable.getWriter();
    reader.releaseLock();
    pumped = writer
      .write(first)
      .then(async () => {
        writer.releaseLock();
        await request.body.pipeTo(fixed.writable);
      })
      .catch((e) => {
        failed = e;
      });
    body = fixed.readable;
  } else {
    body = rest(first, reader, (n, error) => {
      if (error) fail(error);
      if (n - skip > MAX_ENTRY) throw fail(new Fail(413, 'Payload Too Large'));
    });
    if (length >= 0) {
      body = body.pipeThrough(new FixedLengthStream(skip + length));
    }
  }
  const r = await env.STORE.fetch(at, {
    method: 'PUT',
    headers: { 'X-Expires': `${expires}` },
    body,
    signal: abort.signal,
  }).catch((e) => ({ error: e }));
  await pumped;
  if (failed !== null) {
    if (r.ok) await remove(env, at).catch(() => {});
    await drain().catch(() => {});
    throw failed;
  }
  if (r.error) throw r.error;
  if (r.status === 413) throw new Fail(413, 'Payload Too Large');
  if (!r.ok) throw new Error(`Writing the entry failed: ${r.status}.`);
  return new Response(null, { status: 204 });
}

function blankLine(buf) {
  for (let i = 0; i + 3 < buf.byteLength; i++) {
    if (buf[i] === 13 && buf[i + 1] === 10) {
      if (buf[i + 2] === 13 && buf[i + 3] === 10) return i;
    }
  }
  return -1;
}

// =======================================================================
// Storability and lifetime: what Miniflare asks of http-cache-semantics
// 4.1.1, as a shared cache that ignores the request's Cache-Control.

// `{ms, headers}`: how long a response with `status` and `headers` stays
// fresh, and the headers to store; null if it must not be stored.
function lifetime(status, headers, req) {
  const get = (name) => headers.find(([k]) => k.toLowerCase() === name)?.[1];
  const cc = {};
  for (const part of (get('cache-control') ?? '').split(',')) {
    const [k, v] = part.split('=', 2).map((s) => s.trim());
    if (k !== '') cc[k.toLowerCase()] = v?.replace(/^"|"$/g, '') ?? true;
  }
  if (get('cache-control') === undefined && /no-cache/.test(get('pragma'))) {
    cc['no-cache'] = true;
  }
  const drop = new Set(HOP_BY_HOP);
  for (const h of (get('connection') ?? '').split(',')) {
    drop.add(h.trim().toLowerCase());
  }
  if (String(cc.private).toLowerCase() === 'set-cookie') {
    delete cc.private;
    drop.add('set-cookie');
    const other = (p) => !/^\s*private\s*=\s*"?set-cookie"?\s*$/i.test(p);
    const strip = (v) => v.split(',').filter(other).join(',').trim();
    headers = headers.map(([k, v]) => [
      k,
      k.toLowerCase() === 'cache-control' ? strip(v) : v,
    ]);
  }
  const empty = ([k, v]) => v === '' && k.toLowerCase() === 'cache-control';
  headers = headers.filter((h) => !drop.has(h[0].toLowerCase()) && !empty(h));
  const storable =
    KNOWN.includes(status) &&
    !cc['no-store'] &&
    !cc.private &&
    !headers.some(([k]) => k.toLowerCase() === 'set-cookie') &&
    (!req.has('Authorization') ||
      cc['must-revalidate'] ||
      cc.public ||
      cc['s-maxage']) &&
    (get('expires') ||
      cc['max-age'] ||
      cc['s-maxage'] ||
      cc.public ||
      BY_DEFAULT.includes(status));
  if (!storable) return null;
  const num = (v) => (Number.isFinite(parseInt(v)) ? parseInt(v) : 0);
  const fresh = (s) => ({
    ms: Math.max(0, s - num(get('age'))) * 1000,
    headers,
  });
  if (cc['no-cache'] || cc['proxy-revalidate']) return fresh(0);
  if (cc['s-maxage']) return fresh(num(cc['s-maxage']));
  if (cc['max-age']) return fresh(num(cc['max-age']));
  const min = cc.immutable ? 24 * 3600 : 0;
  const date = Date.parse(get('date'));
  const served = Number.isFinite(date) ? date : clock();
  if (get('expires')) {
    const expires = Date.parse(get('expires'));
    if (Number.isNaN(expires) || expires < served) return fresh(0);
    return fresh(Math.max(min, (expires - served) / 1000));
  }
  const modified = Date.parse(get('last-modified'));
  if (Number.isFinite(modified) && served > modified) {
    return fresh(Math.max(min, (served - modified) / 10000));
  }
  return fresh(min);
}

// =======================================================================
// Workers Caching: the `ReadThrough` entrypoint, in front of a Worker.
//
// Its bindings: ORIGIN, the Worker; STORE, a store that purges (a
// cluster's `cache`); NAMESPACE (optional), its entries' among others in
// STORE. The Worker's `cacheApiOutbound` is this module's default export
// with the same bindings, so that `ctx.cache.purge()`, a POST /purge,
// reaches them.
//
// A GET or HEAD is answered from a fresh entry of its path and query
// (`HIT`). Else a GET runs the Worker, and misses of the same key on this
// node join it (`MISS`). A response the headers allow is stored by
// `Cloudflare-CDN-Cache-Control`, else `CDN-Cache-Control`, else as the
// Cache API stores it; 206, 304, 520-526 and any `Vary` never are. Other
// responses stream through (`BYPASS`), as do other requests, upgrades and
// ones with `Authorization`. A fill serves its response while it stores
// it; joiners are served from its body if that is at most SPOOL bytes,
// else look again once it is stored. The entry's `Cache-Tag` and path go
// to the store for purges; neither `Cache-Tag` nor
// `Cloudflare-CDN-Cache-Control` is served.
//
// The store hands a miss a ticket, and refuses the fill's put (412) if a
// purge that matches it began since. Without a ticket nothing is stored.
// Joiners wait on promises their fill resolves, which needs a compatibility
// date of 2024-10-14 or later (`handle_cross_request_promise_resolution`).
//
// TODO(soon): `Vary`, stale-while-revalidate, stale-if-error,
// `invalidate`, `cf.cacheKey`, a cache per entrypoint and version, a disk
// STORE (purges there), one Worker run per key across a cluster.

const SPOOL = 1024 * 1024;
const UNSERVED = ['cache-tag', 'cloudflare-cdn-cache-control'];
// The misses running the Worker, by key.
const fills = new Map();

async function readThrough(request, env, ctx) {
  const h = request.headers;
  if (
    !['GET', 'HEAD'].includes(request.method) ||
    h.has('Upgrade') ||
    h.has('Authorization')
  ) {
    return env.ORIGIN.fetch(request);
  }
  const url = new URL(request.url);
  const path = url.pathname + url.search;
  const ns = env.NAMESPACE ?? '';
  const at = await storeUrl(`\0rt\0${ns}\0${path}`);
  for (;;) {
    const miss = {};
    const entry = await load(env, at, miss);
    if (entry !== null) return answer(request, entry, 'HIT');
    if (request.method === 'HEAD') return env.ORIGIN.fetch(request);
    const ticket = miss.ticket ?? null;
    const fill = fills.get(at);
    if (fill === undefined) {
      return run(request, env, ctx, { at, ns, path, ticket });
    }
    const joined = await join(fill, ticket);
    if (joined !== null) return answer(request, joined, 'MISS');
  }
}

// The fill's entry once it has one in memory, or null to look again once
// it is over. Only a joiner whose miss had the fill's ticket takes it: a
// purge begun on this node between the two, as is every purge that
// returned before the joiner's request, may remove it. Called in the
// joiner's request: streams cannot cross requests.
async function join(fill, ticket) {
  const same = ticket !== null && ticket !== 'none' && ticket === fill.ticket;
  while (!fill.over && !(same && fill.bytes !== null)) {
    await new Promise((resolve) => fill.waiting.push(resolve));
  }
  const { head, bytes } = fill;
  if (!same || bytes === null) return null;
  return { head, body: new Response(bytes).body, size: bytes.byteLength };
}

// Runs the Worker for a miss of `at`, and answers with its response. The
// fill is in waitUntil from the start, so that it ends however this
// request does.
async function run(request, env, ctx, { at, ns, path, ticket }) {
  const fill = { ticket, head: null, bytes: null, waiting: [], over: false };
  const wake = () => fill.waiting.splice(0).forEach((resolve) => resolve());
  fills.set(at, fill);
  const filled = fillFrom(request, env, fill, wake, { at, ns, path, ticket });
  const over = filled.then(
    ({ stored }) => stored,
    () => {}
  );
  ctx.waitUntil(
    over.finally(() => {
      fill.over = true;
      if (fills.get(at) === fill) fills.delete(at);
      wake();
    })
  );
  return (await filled).response;
}

// `{response, stored}`: the Worker's response to serve, and its put.
async function fillFrom(request, env, fill, wake, { at, ns, path, ticket }) {
  const res = await env.ORIGIN.fetch(request, { encodeResponseBody: 'manual' });
  const { status, statusText } = res;
  const policy = cdnLifetime(status, [...res.headers]);
  if (policy === null) {
    const headers = new Headers(res.headers);
    UNSERVED.forEach((name) => headers.delete(name));
    headers.set('CF-Cache-Status', 'BYPASS');
    const response = new Response(res.body, {
      status,
      statusText,
      headers,
      encodeBody: 'manual',
    });
    return { response };
  }
  const expires = Math.min(
    Math.floor(clock() + policy.ms),
    Number.MAX_SAFE_INTEGER
  );
  const head = { status, statusText, headers: policy.headers, expires };
  const names = {
    'X-Expires': `${expires}`,
    'X-Purge-Namespace': ns,
    'X-Purge-Path': path,
    'X-Purge-Tags': res.headers.get('Cache-Tag') ?? '',
  };
  const stores = ticket !== null && ticket !== 'none';
  if (stores) names['X-Ticket'] = ticket;
  const json = enc.encode(JSON.stringify(head));
  const prefix = new Uint8Array(4);
  new DataView(prefix.buffer).setUint32(0, json.byteLength);
  const first = concat(prefix, json);
  const read = await spool(res.body);
  const store = (body, signal) =>
    !stores
      ? Promise.resolve()
      : env.STORE.fetch(at, { method: 'PUT', headers: names, body, signal })
          .then((r) => r.body?.cancel())
          .catch(() => {});
  if (read.bytes !== undefined) {
    fill.head = head;
    fill.bytes = read.bytes;
    wake();
    const stored = store(concat(first, read.bytes));
    const body = new Response(read.bytes).body;
    const entry = { head, body, size: read.bytes.byteLength };
    return { response: await answer(request, entry, 'MISS'), stored };
  }
  // A body that fails must not be stored, but the store may see one
  // without a length that fails as one that ends: so it is cancelled.
  const abort = new AbortController();
  const [mine, theirs] = rest(read.first, read.reader, (_, error) => {
    if (error) abort.abort(error);
  }).tee();
  const stored = store(rest(first, theirs.getReader()), abort.signal);
  const headers = new Headers(head.headers);
  headers.set('CF-Cache-Status', 'MISS');
  const response = new Response(mine, {
    status,
    statusText,
    headers,
    encodeBody: 'manual',
  });
  return { response, stored };
}

// `{bytes}`, `body` whole if it is at most SPOOL bytes, else `{first,
// reader}`: what was read, and the rest.
async function spool(body) {
  if (body === null) return { bytes: new Uint8Array(0) };
  const reader = body.getReader();
  let bytes = new Uint8Array(0);
  for (;;) {
    const { value, done } = await reader.read();
    if (done) return { bytes };
    bytes = concat(bytes, value);
    if (bytes.byteLength > SPOOL) return { first: bytes, reader };
  }
}

// `{ms, headers}` for Workers Caching: as `lifetime`, by
// `Cloudflare-CDN-Cache-Control`, else `CDN-Cache-Control`, else
// `Cache-Control`; null if not stored. `headers` are lowercase.
function cdnLifetime(status, headers) {
  const get = (name) => headers.find(([k]) => k === name)?.[1];
  const never = [206, 304].includes(status) || (status >= 520 && status <= 526);
  if (never || get('vary') !== undefined) return null;
  const cdn = get('cloudflare-cdn-cache-control') ?? get('cdn-cache-control');
  const own = headers.filter(([k]) => k === 'cache-control');
  const asked =
    cdn === undefined
      ? headers
      : [
          ...headers.filter(([k]) => k !== 'cache-control'),
          ['cache-control', cdn],
        ];
  const policy = lifetime(status, asked, new Headers());
  if (policy === null || policy.ms <= 0) return null;
  const served = ([k]) =>
    !UNSERVED.includes(k) && (cdn === undefined || k !== 'cache-control');
  const kept = policy.headers.filter(served);
  return {
    ms: policy.ms,
    headers: cdn === undefined ? kept : [...kept, ...own],
  };
}

// `ctx.cache.purge()`: POST /purge?tag=..&prefix=..&everything, from
// src/workerd/server/workerd-api.c++, passed to the store with this
// module's NAMESPACE.
async function purge(request, env) {
  const url = new URL(request.url);
  url.searchParams.set('ns', env.NAMESPACE ?? '');
  const r = await env.STORE.fetch(`http://store/?${url.searchParams}`, {
    method: 'POST',
  });
  await r.body?.cancel();
  return new Response(null, { status: r.status === 200 ? 200 : 500 });
}
