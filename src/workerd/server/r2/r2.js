// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// R2, on the HTTP protocol workerd's R2 binding speaks (src/workerd/api/
// r2-rpc.c++, r2-api.capnp): GET with the request as JSON in CF-R2-Request
// for head, get and list; PUT with the request as a JSON prefix of the body,
// CF-R2-Metadata-Size bytes long, for the rest. A response's JSON metadata
// prefixes its body, as long as its CF-R2-Metadata-Size; an error is
// CF-R2-Error. The binding's props name the bucket:
//
//   r2Bucket = (name = "<this service>",
//               props = (json = "{\"bucket\": \"<name>\"}"))
//
// Each R2 object is one object in `BLOBS`, a `bucket` service (the
// cluster's bucket, or a local store), at `<bucket>/<key>`. There is no
// index, so the store's listing is the bucket's and a tool reading the
// store sees the objects under their names. The content headers are stored
// as headers, and customMetadata, cacheExpiry, checksums and the storage
// class as one JSON value, escaped to ASCII, in the user metadata
// `workerd-r2`. An object without it reads its user metadata as its
// customMetadata.
//
// The config that runs this module (`bundled = "/workerd/r2.js"`) is an
// interface: cli/wrangler.rs and hand-written configs write it. Anything
// added to it later must be optional. Its bindings:
//
//   BLOBS  a `bucket` service
//   TEST   JSON `true` for tests: multipart parts may be as small as
//          `TEST_MIN_PART` bytes, as in Miniflare, with a store whose
//          `minPartSize` is as small (optional)
//
// A put is one streamed write to the store, which writes up to `PART` bytes
// at once, fenced on the version an `onlyIf` was checked against, and more
// as a multipart upload. Its checksums are checked as it streams, and its
// last chunk is held back until they pass, so a mismatch leaves the body
// short and the store keeps nothing. Where a plain
// object store cannot be R2:
//
//   - a put over `PART` bytes has the etag `md5(md5s)-N` of a multipart
//     upload, and its `onlyIf` is checked when it starts, not fenced;
//   - a part's etag is its MD5, so the same bytes uploaded twice under one
//     part number have one etag, and either is accepted at completion;
//   - aborting an upload the store doesn't know, finished or never begun,
//     succeeds;
//   - completing an upload with no parts writes an empty object without the
//     upload's metadata, with a plain MD5 etag;
//   - the `bucket` service holds a part in memory, so one over 64 MiB is
//     refused;
//   - the record is user metadata, which S3 caps at 2 KB, so on S3 a put
//     with more customMetadata than fits is refused as MetadataTooLarge;
//   - a listing that includes metadata is a HEAD of each object listed;
//     one that doesn't gives the store's time, not the put's, and says
//     every object's storage class is Standard;
//   - a key with a `.` or `..` segment is refused (InvalidObjectName), as
//     stores and proxies may resolve them;
//   - an upload never completed or aborted keeps its parts: a store's
//     lifecycle rule for incomplete multipart uploads collects them.
//
// Limits and error messages follow Miniflare's R2 simulator (workers-sdk,
// packages/miniflare/src/workers/r2). Not supported, as there: SSE-C keys
// (ignored), bucket administration, lifecycle rules.

const PART = 8 * 1024 * 1024;
const MAX_KEY = 1024;
const MAX_VALUE = 5 * 1024 * 1024 * 1024 - 5 * 1024 * 1024;
const MAX_METADATA = 8192;
const MAX_LIST = 1000;
const MIN_PART = 5 * 1024 * 1024;
const TEST_MIN_PART = 50;
// Requests to `BLOBS` in flight at once for a listing's metadata or a
// multi-key delete.
const PARALLEL = 16;
// The user metadata holding the R2 record.
const ENVELOPE = 'workerd-r2';
// capnp's JSON for an unset UInt64 with a default of all ones.
const UNSET = 18446744073709551615n;
// Before the store's token in a list cursor.
const CURSOR = 't.';
// Before the key a list cursor starts after, when a reply stopped short.
const AFTER = 'k.';
// The most a list reply's JSON takes; the binding refuses over 1 MiB.
const MAX_REPLY = 512 * 1024;

const ERRORS = {
  InternalError: [
    500,
    10001,
    'We encountered an internal error. Please try again.',
  ],
  NoSuchKey: [404, 10007, 'The specified key does not exist.'],
  EntityTooLarge: [
    400,
    100100,
    'Your proposed upload exceeds the maximum allowed object size.',
  ],
  EntityTooSmall: [
    400,
    10011,
    'Your proposed upload is smaller than the minimum allowed object size.',
  ],
  MetadataTooLarge: [
    400,
    10012,
    'Your metadata headers exceed the maximum allowed metadata size.',
  ],
  InvalidObjectName: [400, 10020, 'The specified object name is not valid.'],
  InvalidMaxKeys: [
    400,
    10022,
    'MaxKeys params must be positive integer <= 1000.',
  ],
  NoSuchUpload: [400, 10024, 'The specified multipart upload does not exist.'],
  InvalidPart: [
    400,
    10025,
    'One or more of the specified parts could not be found.',
  ],
  InvalidMetadata: [400, 10029, 'Metadata missing or invalid'],
  PreconditionFailed: [
    412,
    10031,
    'At least one of the pre-conditions you specified did not hold.',
  ],
  BadDigest: [400, 10037, ''],
  InvalidRange: [416, 10039, 'The requested range is not satisfiable'],
  BadUpload: [500, 10048, 'There was a problem with the multipart upload.'],
};

// An R2 error by its name in `ERRORS`; `object` is the metadata a failed
// get's precondition answers with.
class Fail extends Error {
  constructor(name, message = ERRORS[name][2], object = null) {
    super(message);
    this.code = name;
    this.object = object;
  }
}

function failure(e) {
  if (!(e instanceof Fail)) {
    console.error('R2:', e);
    e = new Fail('InternalError');
  }
  const [status, v4code] = ERRORS[e.code];
  const headers = {
    'CF-R2-Error': JSON.stringify({ version: 1, v4code, message: e.message }),
  };
  if (e.object == null) return new Response(null, { status, headers });
  const meta = utf8Bytes(JSON.stringify(e.object));
  headers['CF-R2-Metadata-Size'] = String(meta.byteLength);
  return new Response(meta, { status, headers });
}

export default {
  async fetch(request, env, ctx) {
    const name = ctx.props?.bucket;
    if (typeof name !== 'string' || name === '') {
      return new Response('No bucket: props must be {"bucket": "<name>"}.', {
        status: 400,
      });
    }
    const bucket = { env, name };
    try {
      return await serve(request, bucket);
    } catch (e) {
      return failure(e);
    }
  },
};

async function serve(request, bucket) {
  if (request.method === 'GET') {
    const req = parse(utf8Bytes(request.headers.get('CF-R2-Request') ?? ''));
    switch (req.method) {
      case 'head': {
        const o = await head(bucket, key(req.object));
        if (o === null) throw new Fail('NoSuchKey');
        return metadata(await describe(o, { offset: 0, length: o.size }));
      }
      case 'get':
        return get(bucket, req);
      case 'list':
        return metadata(await list(bucket, req));
    }
    throw new Fail('InternalError');
  }
  if (request.method !== 'PUT') return new Response(null, { status: 405 });
  const n = parseInt(request.headers.get('CF-R2-Metadata-Size'));
  if (!(n >= 0)) throw new Fail('InvalidMetadata');
  const take = reader(request.body);
  try {
    const req = parse(await take(n));
    // A put needs its length.
    const size = Number(request.headers.get('Content-Length') ?? NaN) - n;
    if (!(size >= 0)) throw new Fail('InvalidMetadata');
    return await write(bucket, req, take, size);
  } finally {
    // The binding sends the whole body before it reads the response.
    await take.drain();
  }
}

async function write(bucket, req, take, size) {
  switch (req.method) {
    case 'put':
      return metadata(await put(bucket, req, take, size));
    case 'delete': {
      const keys = (req.objects ?? [req.object]).map(key);
      await parallel(keys, async (k) => {
        const r = await blobs(bucket, k, { method: 'DELETE' });
        if (!r.ok) throw new Error(`DELETE ${k}: ${r.status}`);
      });
      return new Response(null);
    }
    case 'createMultipartUpload': {
      const k = key(req.object);
      const r = await blobs(bucket, k, {
        method: 'POST',
        query: 'uploads',
        headers: headers(envelope(req)),
      });
      const { uploadId } = await answer(r, `create ${k}`);
      return metadata({ uploadId });
    }
    case 'uploadPart':
      return metadata(await uploadPart(bucket, req, take, size));
    case 'completeMultipartUpload':
      return metadata(await complete(bucket, req));
    case 'abortMultipartUpload': {
      const k = key(req.object);
      const r = await blobs(bucket, k, {
        method: 'DELETE',
        query: `upload=${encodeURIComponent(req.uploadId)}`,
      });
      if (!r.ok && (await code(r)) !== 'NoSuchUpload') {
        throw new Error(`abort ${k}: ${r.status}`);
      }
      return new Response(null);
    }
  }
  throw new Fail('InternalError');
}

function parse(bytes) {
  try {
    const req = JSON.parse(utf8(bytes));
    if (typeof req === 'object' && req !== null) return req;
  } catch {}
  throw new Fail('InvalidMetadata');
}

function key(k) {
  if (typeof k !== 'string') throw new Fail('InvalidMetadata');
  if (utf8Bytes(k).byteLength > MAX_KEY) throw new Fail('InvalidObjectName');
  // A `.` or `..` segment could climb out of the bucket's directory in a
  // store or proxy that resolves them; the bucket service refuses it too.
  if (k.split('/').some((s) => s === '.' || s === '..')) {
    throw new Fail('InvalidObjectName');
  }
  return k;
}

// A UInt64 field of the request, undefined if unset.
function u64(v) {
  if (v === undefined || v === null || BigInt(v) === UNSET) return undefined;
  return Number(v);
}

function metadata(value) {
  const body = utf8Bytes(JSON.stringify(value));
  return new Response(body, {
    headers: { 'CF-R2-Metadata-Size': String(body.byteLength) },
  });
}

// =======================================================================
// The store

// A request to `BLOBS` for `k` in the bucket, or for the bucket itself
// when `k` is null.
function blobs(bucket, k, { method = 'GET', query, headers, body } = {}) {
  const path =
    k === null
      ? ''
      : encodeURIComponent(`${encodeURIComponent(bucket.name)}/${k}`);
  const url = `http://blobs/${path}${query === undefined ? '' : `?${query}`}`;
  // The stored bytes, not decoded by their Content-Encoding.
  const encodeResponseBody = 'manual';
  return bucket.env.BLOBS.fetch(url, {
    method,
    headers,
    body,
    encodeResponseBody,
  });
}

// A store answer's JSON body, or its error as an R2 one.
async function answer(r, what) {
  if (r.ok) {
    const text = await r.text();
    return text === '' ? null : JSON.parse(text);
  }
  const c = await code(r);
  if (c in ERRORS) throw new Fail(c);
  throw new Error(`${what}: ${r.status} ${c}`);
}

async function code(r) {
  try {
    return (await r.json()).code ?? '';
  } catch {
    return '';
  }
}

// The object `k` as the store answers it, or null: its size, version
// (which guards a write), etag, upload time in ms, content headers and user
// metadata.
async function head(bucket, k) {
  const r = await blobs(bucket, k, { method: 'HEAD' });
  if (r.status === 404) return null;
  if (!r.ok) throw new Error(`HEAD ${k}: ${r.status}`);
  return object(k, r.headers, Number(r.headers.get('Content-Length')));
}

function object(k, h, size) {
  const meta = {};
  for (const [name, value] of h) {
    if (name.startsWith('x-meta-')) meta[name.slice(7)] = value;
  }
  return {
    key: k,
    size,
    version: h.get('x-version'),
    etag: (h.get('ETag') ?? '').replace(/^(W\/)?"|"$/g, ''),
    // The write's own time, which the record keeps, else the store's.
    uploaded: recorded(meta)?.uploaded ?? Number(h.get('x-modified')),
    headers: Object.fromEntries(
      HTTP_FIELDS.map(([field, header]) => [field, h.get(header) ?? undefined])
    ),
    meta,
  };
}

const HTTP_FIELDS = [
  ['contentType', 'Content-Type'],
  ['contentLanguage', 'Content-Language'],
  ['contentDisposition', 'Content-Disposition'],
  ['contentEncoding', 'Content-Encoding'],
  ['cacheControl', 'Cache-Control'],
];

// The R2 record a put or a multipart upload is created with, refused if
// its customMetadata is too large.
function envelope(req, checksums = {}) {
  const http = {};
  for (const [field] of [...HTTP_FIELDS, ['cacheExpiry']]) {
    const v = req.httpFields?.[field];
    if (v !== undefined && v !== '') http[field] = v;
  }
  if ('cacheExpiry' in http) http.cacheExpiry = u64(http.cacheExpiry);
  const custom = Object.fromEntries(
    (req.customFields ?? []).map(({ k, v }) => [k, v])
  );
  let length = 0;
  for (const [k, v] of Object.entries(custom)) {
    length += serialisedLength(k) + serialisedLength(v);
  }
  if (length > MAX_METADATA) throw new Fail('MetadataTooLarge');
  return {
    custom,
    http,
    checksums,
    storageClass: req.storageClass || 'Standard',
  };
}

// As R2 counts metadata.
function serialisedLength(s) {
  for (let i = 0; i < s.length; i++) {
    if (s.charCodeAt(i) >= 256) return s.length * 2;
  }
  return s.length;
}

// The headers that store `record` with an object. The content fields are
// also headers, for tools that read the store, when they can be: the record
// is what counts.
function headers(record) {
  const out = {};
  for (const [field, header] of HTTP_FIELDS) {
    const v = record.http[field];
    if (typeof v === 'string' && /^[\t\x20-\x7e]*$/.test(v)) out[header] = v;
  }
  const json = JSON.stringify(record).replace(
    /[\u007f-￿]/g,
    (c) => `\\u${c.charCodeAt(0).toString(16).padStart(4, '0')}`
  );
  out[`x-meta-${ENVELOPE}`] = json;
  return out;
}

// The record in user metadata `meta`, if any.
function recorded(meta) {
  try {
    const record = JSON.parse(meta[ENVELOPE]);
    if (typeof record === 'object' && record !== null) return record;
  } catch {}
  return null;
}

// The R2 record of a stored object: its envelope, or for an object written
// by something else, its user metadata and headers.
function stored(o) {
  const record = recorded(o.meta);
  if (record) return record;
  const http = {};
  for (const [field] of HTTP_FIELDS) {
    if (o.headers[field] !== undefined) http[field] = o.headers[field];
  }
  return { custom: o.meta, http, checksums: {}, storageClass: 'Standard' };
}

const ALGORITHMS = [
  ['md5', 'MD5'],
  ['sha1', 'SHA-1'],
  ['sha256', 'SHA-256'],
  ['sha384', 'SHA-384'],
  ['sha512', 'SHA-512'],
];

// An object as the binding's R2HeadResponse.
async function describe(o, range, include = { http: true, custom: true }) {
  const record = o.meta === undefined ? {} : stored(o);
  const checksums = {};
  if (/^[0-9a-f]{32}$/.test(o.etag)) checksums[0] = o.etag;
  for (const [i, [field]] of ALGORITHMS.entries()) {
    if (i > 0 && record.checksums?.[field])
      checksums[i] = record.checksums[field];
  }
  return {
    name: o.key,
    // The store's version names the bytes this key holds now; R2's is 32
    // hex digits.
    version: hex(
      new Uint8Array(await crypto.subtle.digest('MD5', utf8Bytes(o.version)))
    ),
    size: o.size,
    etag: o.etag,
    uploaded: o.uploaded,
    httpFields: include.http ? (record.http ?? {}) : {},
    customFields: include.custom
      ? Object.entries(record.custom ?? {}).map(([k, v]) => ({ k, v }))
      : [],
    range,
    checksums,
    storageClass: record.storageClass ?? 'Standard',
  };
}

// =======================================================================
// Reads

async function get(bucket, req) {
  const k = key(req.object);
  let checked = null;
  if (req.onlyIf) {
    checked = await head(bucket, k);
    if (checked === null) throw new Fail('NoSuchKey');
    if (!passes(req.onlyIf, checked)) {
      const whole = { offset: 0, length: checked.size };
      throw new Fail(
        'PreconditionFailed',
        undefined,
        await describe(checked, whole)
      );
    }
  }
  const range = toRange(req);
  const r = await blobs(bucket, k, {
    headers: range === undefined ? {} : { Range: range },
  });
  if (r.status === 404) throw new Fail('NoSuchKey');
  if (r.status === 416) {
    // Past the end, as Miniflare reads it: a Range header's is all of the
    // value, and a range starting at its end is none of it.
    if (req.rangeHeader) return get(bucket, { ...req, rangeHeader: undefined });
    const o = await head(bucket, k);
    if (o === null) throw new Fail('NoSuchKey');
    if ((u64(req.range?.offset) ?? 0) !== o.size)
      throw new Fail('InvalidRange');
    return metadata(await describe(o, { offset: o.size, length: 0 }));
  }
  if (!r.ok) throw new Error(`GET ${k}: ${r.status}`);
  const length = Number(r.headers.get('Content-Length'));
  // Content-Range: bytes <first>-<last>/<size>
  const m = /bytes (\d+)-\d+\/(\d+)/.exec(r.headers.get('Content-Range') ?? '');
  const size = m === null ? length : Number(m[2]);
  const o = object(k, r.headers, size);
  // The precondition held for the version it was checked against.
  if (checked !== null && o.version !== checked.version) {
    r.body?.cancel();
    return get(bucket, req);
  }
  const offset = m === null ? 0 : Number(m[1]);
  const meta = utf8Bytes(JSON.stringify(await describe(o, { offset, length })));
  const { readable, writable } = new FixedLengthStream(
    meta.byteLength + length
  );
  const w = writable.getWriter();
  (async () => {
    await w.write(meta);
    w.releaseLock();
    if (r.body) await r.body.pipeTo(writable);
    else await writable.close();
  })().catch((e) => writable.abort(e).catch(() => {}));
  return new Response(readable, {
    headers: { 'CF-R2-Metadata-Size': String(meta.byteLength) },
  });
}

// The Range header for the request's range, or undefined for all of it. A
// Range header with other than one range reads all of it, as Miniflare
// does.
function toRange(req) {
  if (req.rangeHeader) {
    const m = /^\s*bytes\s*=\s*(\d*)\s*-\s*(\d*)\s*$/i.exec(req.rangeHeader);
    if (m === null || (m[1] === '' && m[2] === '')) return undefined;
    if (m[1] === '' && Number(m[2]) === 0) return undefined;
    if (m[1] !== '' && m[2] !== '' && Number(m[1]) > Number(m[2])) {
      return undefined;
    }
    return `bytes=${m[1]}-${m[2]}`;
  }
  const r = req.range;
  if (r === undefined) return undefined;
  const offset = u64(r.offset);
  const length = u64(r.length);
  const suffix = u64(r.suffix);
  if (suffix !== undefined) {
    if (suffix <= 0) throw new Fail('InvalidRange');
    return `bytes=-${suffix}`;
  }
  if (offset === undefined && length === undefined) return undefined;
  if (length !== undefined && length <= 0) throw new Fail('InvalidRange');
  const first = offset ?? 0;
  return `bytes=${first}-${length === undefined ? '' : first + length - 1}`;
}

// Whether an object passes an R2Conditional, as R2's gateway decides
// (RFC 7232, section 6), after Miniflare's `_testR2Conditional`.
function passes(cond, o) {
  const matches = cond.etagMatches?.length ? cond.etagMatches : undefined;
  const noneMatch = cond.etagDoesNotMatch?.length
    ? cond.etagDoesNotMatch
    : undefined;
  const before = u64(cond.uploadedBefore);
  const after = u64(cond.uploadedAfter);
  if (o === null) return matches === undefined && after === undefined;
  const has = (list, weak) =>
    list.some(
      (c) =>
        c.type === 'wildcard' ||
        (c.value === o.etag && (c.type === 'strong' || weak))
    );
  const t = (ms) =>
    cond.secondsGranularity ? Math.floor(ms / 1000) * 1000 : ms;
  const ifMatch = matches === undefined || has(matches, false);
  const ifNoneMatch = noneMatch === undefined || !has(noneMatch, true);
  const uploaded = t(o.uploaded);
  const ifModifiedSince =
    after === undefined ||
    t(after) < uploaded ||
    (noneMatch !== undefined && ifNoneMatch);
  const ifUnmodifiedSince =
    before === undefined ||
    uploaded < t(before) ||
    (matches !== undefined && ifMatch);
  return ifMatch && ifNoneMatch && ifModifiedSince && ifUnmodifiedSince;
}

async function list(bucket, req) {
  const limit =
    req.limit === undefined || req.limit === 0xffffffff ? MAX_LIST : req.limit;
  if (limit < 1 || limit > MAX_LIST) throw new Fail('InvalidMaxKeys');
  // A cursor is the store's token or the key a short reply stopped after;
  // another lists nothing, as in Miniflare.
  const cursor = req.cursor ?? '';
  const token = cursor.startsWith(CURSOR) && cursor.slice(CURSOR.length);
  const after = cursor.startsWith(AFTER) ? cursor.slice(AFTER.length) : null;
  if (cursor && !token && after === null) {
    return { objects: [], truncated: false, delimitedPrefixes: [] };
  }
  const startAfter = after ?? req.startAfter;
  const include = (req.include ?? []).map(Number);
  const wanted = { http: include.includes(0), custom: include.includes(1) };
  const root = `${encodeURIComponent(bucket.name)}/`;
  const query = new URLSearchParams({
    list: '',
    prefix: root + (req.prefix ?? ''),
    max: String(limit),
  });
  if (startAfter) query.set('start-after', root + startAfter);
  if (token) query.set('token', token);
  if (req.delimiter) query.set('delimiter', req.delimiter);
  const r = await blobs(bucket, null, { query: query.toString() });
  const page = await answer(r, `list ${bucket.name}`);
  const strip = (k) => k.slice(root.length);
  const entries = page.objects.map((e) => ({
    key: strip(e.key),
    size: e.size,
    etag: e.etag,
    version: e.version,
    uploaded: e.modified,
  }));
  // Metadata the listing doesn't carry is a HEAD of each object.
  let objects = await parallel(entries, async (e) => {
    if (!wanted.http && !wanted.custom) return describe(e, undefined, wanted);
    return describe((await head(bucket, e.key)) ?? e, undefined, wanted);
  });
  let delimitedPrefixes = page.prefixes.map(strip);
  let next = page.token == null ? undefined : CURSOR + page.token;
  // Fewer than `limit` when the reply would be too large, as R2 does: the
  // objects and prefixes in the store's order, until their JSON's bytes
  // pass the budget.
  const items = [
    ...objects.map((o) => [o.name, o]),
    ...delimitedPrefixes.map((p) => [p, null]),
  ].sort(([a], [b]) => compare(a, b));
  let size = 0;
  const over = items.findIndex(
    ([p, o]) => (size += utf8Bytes(JSON.stringify(o ?? p)).length) > MAX_REPLY
  );
  const kept = items.slice(0, Math.max(1, over));
  if (over !== -1 && kept.length < items.length) {
    objects = kept.filter(([, o]) => o).map(([, o]) => o);
    delimitedPrefixes = kept.filter(([, o]) => !o).map(([p]) => p);
    // After a prefix is after every key in it.
    const [last, o] = kept.at(-1);
    next = AFTER + (o ? last : last + '\u{10FFFF}');
  }
  const result = { objects, truncated: next !== undefined, delimitedPrefixes };
  if (next !== undefined) result.cursor = next;
  return result;
}

// =======================================================================
// Writes

async function put(bucket, req, take, size) {
  const k = key(req.object);
  if (size > MAX_VALUE) throw new Fail('EntityTooLarge');
  // The checksums are verified as the value streams to the store, which
  // keeps it only if its stream ends rather than fails: the record can
  // name them before the value is sent.
  const checksums = {};
  for (const [field] of ALGORITHMS.slice(1)) {
    if (req[field]) checksums[field] = req[field].toLowerCase();
  }
  const record = envelope(req, checksums);
  // The value is not sent for a precondition that fails already. The store
  // fences a write of one part on the version it was checked against.
  let guard = {};
  if (req.onlyIf) {
    const o = await head(bucket, k);
    if (!passes(req.onlyIf, o)) throw new Fail('PreconditionFailed');
    if (size <= PART) {
      guard = o === null ? { 'If-None-Match': '*' } : { 'If-Match': o.version };
    }
  }
  const check = checked(req);
  const bad = () => check.bad;
  // An empty value has no last chunk to hold back.
  if (size === 0) await check.end();
  const body = take.stream(size, size === 0 ? null : check);
  record.uploaded = Date.now();
  let r;
  try {
    r = await blobs(bucket, k, {
      method: 'PUT',
      headers: { ...headers(record), ...guard },
      body,
    });
  } catch (e) {
    throw bad() ?? e;
  }
  if (bad()) throw bad();
  if (r.status === 412) throw new Fail('PreconditionFailed');
  // A store's refusal, such as S3's limit on user metadata, is R2's error.
  if (!r.ok) await answer(r, `PUT ${k}`);
  return describe(written(k, r.headers, size, record), undefined);
}

// The object a write's answer describes.
function written(k, h, size, record) {
  const o = object(k, h, size);
  const meta = { [ENVELOPE]: JSON.stringify(record) };
  return { ...o, uploaded: record.uploaded ?? o.uploaded, meta };
}

// The hashes of a put's value as it streams, refused at its end unless it
// has the checksums the put names; `bad` is the refusal.
function checked(req) {
  const hashes = ALGORITHMS.filter(([field]) => req[field]).map(
    ([field, name]) => {
      const stream = new crypto.DigestStream(name);
      return { field, name, stream, writer: stream.getWriter() };
    }
  );
  const check = {
    bad: null,
    async write(chunk) {
      for (const h of hashes) await h.writer.write(chunk);
    },
    async end() {
      for (const { field, name, stream, writer } of hashes) {
        await writer.close();
        const got = hex(new Uint8Array(await stream.digest));
        const given = field === 'md5' ? hex(unbase64(req[field])) : req[field];
        if (given.toLowerCase() === got) continue;
        check.bad = new Fail(
          'BadDigest',
          `The ${name} checksum you specified did not match what we ` +
            `received.\nYou provided a ${name} checksum with value: ` +
            `${given}\nActual ${name} was: ${got}`
        );
        throw check.bad;
      }
    },
  };
  return check;
}

// An upload's parts, `[{part, etag, size}]` in part order.
async function parts(bucket, k, uploadId) {
  const r = await blobs(bucket, k, {
    query: `upload=${encodeURIComponent(uploadId)}`,
  });
  return answer(r, `parts ${k}`);
}

async function uploadPart(bucket, req, take, size) {
  const k = key(req.object);
  const r = await blobs(bucket, k, {
    method: 'PUT',
    query: `part=${req.partNumber}&upload=${encodeURIComponent(req.uploadId)}`,
    body: take.stream(size),
  });
  const { etag } = await answer(r, `part ${k}`);
  return { etag };
}

// Completes an upload with the chosen parts, checked as R2 checks them:
// unknown parts, too small ones in the order given, then parts but the
// last of unequal size.
async function complete(bucket, req) {
  const k = key(req.object);
  const chosen = req.parts ?? [];
  if (new Set(chosen.map((p) => p.part)).size !== chosen.length) {
    throw new Fail('InternalError');
  }
  let uploaded;
  try {
    uploaded = new Map(
      (await parts(bucket, k, req.uploadId)).map((p) => [p.part, p])
    );
  } catch (e) {
    // An upload this key never had, as Miniflare answers it.
    if (e instanceof Fail && e.code === 'NoSuchUpload' && chosen.length === 0) {
      throw new Fail('InternalError');
    }
    throw e;
  }
  const picked = chosen.map(({ part, etag }) => {
    const p = uploaded.get(part);
    if (p === undefined || p.etag !== etag) throw new Fail('InvalidPart');
    return p;
  });
  const min = bucket.env.TEST ? TEST_MIN_PART : MIN_PART;
  if (picked.slice(0, -1).some((p) => p.size < min)) {
    throw new Fail('EntityTooSmall');
  }
  picked.sort((a, b) => a.part - b.part);
  const size = picked[0]?.size;
  if (
    picked.slice(0, -1).some((p) => p.size !== size) ||
    picked.at(-1)?.size > size
  ) {
    throw new Fail('BadUpload');
  }
  const upload = `upload=${encodeURIComponent(req.uploadId)}`;
  if (picked.length === 0) {
    // A store can't complete an upload of no parts.
    const r = await blobs(bucket, k, {
      method: 'PUT',
      body: new Uint8Array(0),
    });
    if (!r.ok) throw new Error(`PUT ${k}: ${r.status}`);
    await blobs(bucket, k, { method: 'DELETE', query: upload });
  } else {
    const r = await blobs(bucket, k, {
      method: 'POST',
      query: upload,
      body: JSON.stringify(picked.map(({ part, etag }) => ({ part, etag }))),
    });
    await answer(r, `complete ${k}`);
  }
  // The upload's record is on the object now.
  const o = await head(bucket, k);
  if (o === null) throw new Fail('InternalError');
  return describe(o, undefined);
}

// =======================================================================
// Helpers

// `fn` of each item, `PARALLEL` at a time, in order.
async function parallel(items, fn) {
  const out = new Array(items.length);
  let next = 0;
  const run = async () => {
    while (next < items.length) {
      const i = next++;
      out[i] = await fn(items[i]);
    }
  };
  await Promise.all(Array.from({ length: PARALLEL }, run));
  return out;
}

// A function that reads up to `n` bytes of `stream`, fewer only at its end,
// copying them only when they span the stream's chunks; its `drain()` reads
// the rest.
function reader(stream) {
  const r = stream?.getReader();
  let rest = new Uint8Array(0);
  const take = async (n) => {
    if (rest.byteLength === 0 && r) {
      const { done, value } = await r.read();
      if (!done) rest = value;
    }
    if (rest.byteLength >= n) {
      const out = rest.subarray(0, n);
      rest = rest.subarray(n);
      return out;
    }
    const out = new Uint8Array(n);
    let at = 0;
    while (at < n) {
      if (rest.byteLength === 0) {
        const { done, value } = r ? await r.read() : { done: true };
        if (done) break;
        rest = value;
      }
      const k = Math.min(n - at, rest.byteLength);
      out.set(rest.subarray(0, k), at);
      rest = rest.subarray(k);
      at += k;
    }
    return out.subarray(0, at);
  };
  // The rest, `size` bytes, as a stream. With `check`, its last chunk is
  // sent only if `check` passes: otherwise the stream fails short of its
  // length, and the store keeps nothing.
  take.stream = (size, check) => {
    const { readable, writable } = new FixedLengthStream(size);
    const w = writable.getWriter();
    (async () => {
      let held = null;
      for (let b; (b = await take(64 * 1024)).byteLength > 0;) {
        await check?.write(b);
        if (held) await w.write(held);
        held = b.slice();
      }
      await check?.end();
      if (held) await w.write(held);
      await w.close();
    })().catch((e) => w.abort(e).catch(() => {}));
    return readable;
  };
  take.drain = async () => {
    while (r && !(await r.read()).done);
  };
  return take;
}

const hex = (bytes) =>
  Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

const unbase64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

const utf8 = (bytes) => new TextDecoder('utf-8', { fatal: true }).decode(bytes);

const utf8Bytes = (s) => new TextEncoder().encode(s);

// `a` against `b` in UTF-8 byte order, as stores sort keys.
function compare(a, b) {
  const [x, y] = [utf8Bytes(a), utf8Bytes(b)];
  const i = x.findIndex((c, i) => c !== y[i]);
  return i === -1 ? x.length - y.length : x[i] - (y[i] ?? -1);
}
