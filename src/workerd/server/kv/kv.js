// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// KV, on the protocol workerd's KV binding speaks (src/workerd/api/kv.c++):
// GET, PUT and DELETE on /<key>, GET /?prefix= for a page of keys, POST
// /bulk/get. The binding's props name the namespace:
//
//   kvNamespace = (name = "<this service>",
//                  props = (json = "{\"namespace\": \"<id>\"}"))
//
// The config that runs this module (`bundled = "/workerd/kv.js"`) is an
// interface: cli/wrangler.rs, Miniflare and hand-written configs write it,
// each for the workerd it ships with or runs on. Anything added to it later
// must be optional, so that configs written before keep working. It is:
//
//   - a Worker with RPC: a compatibility date of 2024-04-03 or later, or
//     the `rpc` flag;
//   - the Durable Object class `KVNamespace`, SQLite-backed (`enableSql`),
//     stored on a local disk. Its `uniqueKey` and that disk are where the
//     namespaces live: a config that changes either starts empty;
//   - these bindings:
//
//   SHARD       the `KVNamespace` class
//   BLOBS       values over `INLINE` bytes: a writable `disk`, or in a
//               cluster a `bucket` service
//   BLOB_CACHE  in a cluster, a writable `disk` on the node that caches
//               blobs (optional)
//   CLUSTERED   JSON `true` in a cluster (optional)
//   TEST_CLOCK  JSON `true` for tests (optional)
//
// A namespace is a Durable Object (`KVNamespace`, by `idFromName`) whose
// SQLite table `entries` is its index: each key's expiration, metadata,
// and its value inline up to `INLINE` bytes or else the path of a blob,
// `<dir>/b/<id>` in `BLOBS`. A blob is immutable: it is reserved, written,
// then claimed by its row's write, and deleted after the row stops naming
// it, through the object's `garbage` table and its alarm. One object holds
// a namespace, so its throughput is one object's.
//
// This Worker reads and writes blobs itself, so large values do not pass
// through the object. In a cluster it keeps a per-process cache of rows and
// misses for each read's `cacheTtl`, which this process's writes update and
// nothing else invalidates. Locally it keeps none: the object is in the
// process, and another process on the same storage, such as `wrangler kv
// key put --local`, must be seen at once. Rows name blobs rather than hold
// them, so entries are small; blobs never change, so `BLOB_CACHE` needs no
// invalidation.
//
// With `TEST_CLOCK`, POST /clock?advance=<ms> moves this process's clock for
// expiry and the entry cache, for tests; blob collection keeps the real
// clock.

import { DurableObject } from 'cloudflare:workers';

const INLINE = 64 * 1024;
const MAX_VALUE = 25 * 1024 * 1024;
const MAX_METADATA = 1024;
const MAX_KEY = 512;
const MAX_BULK_BYTES = 25 * 1024 * 1024;
const MAX_LIST = 1000;
const MAX_BULK = 100;
const MIN_EXPIRATION_TTL = 60;
const MIN_CACHE_TTL = 30;
const CACHE_TTL = 60;
const CACHE_BYTES = 64 * 1024 * 1024;
// A reserved blob unclaimed this long is deleted.
const RESERVE_MS = 600_000;
// A blob no row names is deleted this much later, which spares readers
// that read the row a moment before a retry; less under `TEST_CLOCK`.
const GRACE_MS = 2_000;
const TEST_GRACE_MS = 100;
const READS = 3;
const BLOB_CACHE_BYTES = 1024 * 1024 * 1024;

class Fail extends Error {
  constructor(status, text) {
    super(text);
    this.status = status;
  }
}

const fail = (status, text) =>
  new Response(null, { status, statusText: text.replace(/[\r\n]/g, ' ') });

let clustered;
let hasRpc = false;
let skew = 0;
const clock = () => Date.now() + skew;

export default {
  async fetch(request, env, ctx) {
    clustered ??= env.CLUSTERED === true;
    const ns = ctx.props?.namespace;
    if (typeof ns !== 'string') {
      return fail(400, 'No namespace: props must be {"namespace": "<id>"}.');
    }
    // Without RPC every call to the object fails as "not a function".
    hasRpc ||=
      typeof env.SHARD.get(env.SHARD.idFromName(ns)).read === 'function';
    if (!hasRpc) {
      return fail(
        500,
        'KV needs RPC: give its Worker a compatibility date of 2024-04-03 ' +
          'or later, or the "rpc" compatibility flag.'
      );
    }
    try {
      return await serve(request, env, ns);
    } catch (e) {
      return fail(e.status ?? 500, String(e.message ?? e));
    }
  },
};

async function serve(request, env, ns) {
  const url = request.url;
  const rest = url.slice(url.indexOf('://') + 3);
  const at = rest.search(/[/?]/);
  const target = at < 0 ? '' : rest.slice(at);
  const q = target.indexOf('?');
  const path = (q < 0 ? target : target.slice(0, q)) || '/';
  const params = new URLSearchParams(q < 0 ? '' : target.slice(q + 1));
  const id = env.SHARD.idFromName(ns);
  const space = { ns, dir: id.toString(), shard: env.SHARD.get(id) };
  let key;
  try {
    key = decodeURIComponent(path.slice(1));
  } catch {
    throw new Fail(400, 'Invalid key encoding');
  }
  switch (request.method) {
    case 'POST':
      if (path === '/bulk/get') return bulk(request, env, space);
      if (path === '/clock' && env.TEST_CLOCK) {
        const advance = Number(params.get('advance'));
        if (!Number.isFinite(advance)) {
          return fail(400, 'advance must be a number of milliseconds.');
        }
        skew += advance;
        return new Response(null);
      }
      break;
    case 'GET':
      if (path === '/') return list(space, params);
      return get(env, space, key, params);
    case 'PUT':
      return put(request, env, space, key, params);
    case 'DELETE':
      remember(space, key, null, CACHE_TTL, await space.shard.remove(key));
      return new Response(null);
  }
  return fail(405, 'Method Not Allowed');
}

function cacheTtl(value) {
  if (value === null || value === undefined) return CACHE_TTL;
  const ttl = Number(value);
  if (!Number.isInteger(ttl) || ttl < MIN_CACHE_TTL) {
    throw new Fail(
      400,
      `Invalid cache_ttl of ${value}. Cache TTL must be at least ` +
        `${MIN_CACHE_TTL}.`
    );
  }
  return ttl;
}

async function get(env, space, key, params) {
  const ttl = cacheTtl(params.get('cache_ttl'));
  const { entry, hit } = (await read(env, space, [key], ttl))[0];
  const headers = { 'CF-Cache-Status': hit ? 'HIT' : 'MISS' };
  if (entry === null) return new Response(null, { status: 404, headers });
  if (entry.metadata !== null) headers['CF-KV-Metadata'] = entry.metadata;
  return new Response(entry.value, { headers });
}

// A key's validity, as a single get checks it in the binding.
function validate(key) {
  if (key === '') throw new Fail(400, 'Key names must not be empty');
  if (key === '.' || key === '..') {
    throw new Fail(
      400,
      `Illegal key name "${key}". Please use a different name.`
    );
  }
  validateLength(key);
}

function validateLength(key) {
  const n = bytes(key);
  if (n > MAX_KEY) {
    throw new Fail(
      414,
      `UTF-8 encoded length of ${n} exceeds key length limit of ${MAX_KEY}.`
    );
  }
}

const bytes = (s) => new TextEncoder().encode(s).byteLength;

async function put(request, env, space, key, params) {
  const value = new Uint8Array(await request.arrayBuffer());
  if (value.byteLength > MAX_VALUE) {
    throw new Fail(
      413,
      `Value length of ${value.byteLength} exceeds limit of ${MAX_VALUE}.`
    );
  }
  const expiration = expires(params);
  const metadata = request.headers.get('CF-KV-Metadata');
  if (metadata !== null && bytes(metadata) > MAX_METADATA) {
    throw new Fail(
      413,
      `Metadata length of ${bytes(metadata)} exceeds limit of ` +
        `${MAX_METADATA}.`
    );
  }
  let blob = null;
  if (value.byteLength > INLINE) {
    blob = `${space.dir}/b/${hex(crypto.getRandomValues(new Uint8Array(32)))}`;
    await space.shard.reserve(key, blob);
    const r = await env.BLOBS.fetch(`http://blobs/${blob}`, {
      method: 'PUT',
      body: value,
    });
    if (!r.ok) throw new Error(`Writing the value failed: ${r.status}.`);
    await cacheBlob(env, blob, value);
  }
  const inline = blob === null ? value : null;
  const version = await space.shard.write(
    key,
    inline,
    blob,
    expiration,
    metadata
  );
  const row = { value: inline, blob, expiration, metadata };
  remember(space, key, row, CACHE_TTL, version);
  return new Response(null);
}

// An `expiration` or `expiration_ttl` parameter as a time in ms, parsed
// and refused as production does.
function expires(params) {
  const now = Math.floor(clock() / 1000);
  const ttlText = params.get('expiration_ttl');
  if (ttlText !== null) {
    const ttl = parseInt(ttlText);
    const bad = `Invalid expiration_ttl of ${ttlText}.`;
    if (Number.isNaN(ttl) || ttl <= 0) {
      throw new Fail(400, `${bad} Please specify integer greater than 0.`);
    }
    if (ttl < MIN_EXPIRATION_TTL) {
      throw new Fail(
        400,
        `${bad} Expiration TTL must be at least ${MIN_EXPIRATION_TTL}.`
      );
    }
    return (now + ttl) * 1000;
  }
  const atText = params.get('expiration');
  if (atText === null) return null;
  const at = parseInt(atText);
  const bad = `Invalid expiration of ${atText}.`;
  if (Number.isNaN(at) || at <= now) {
    throw new Fail(
      400,
      `${bad} Please specify integer greater than the current number of ` +
        'seconds since the UNIX epoch.'
    );
  }
  if (at < now + MIN_EXPIRATION_TTL) {
    throw new Fail(
      400,
      `${bad} Expiration times must be at least ${MIN_EXPIRATION_TTL} ` +
        'seconds in the future.'
    );
  }
  return at * 1000;
}

async function list(space, params) {
  const text = params.get('key_count_limit');
  const limit = text === null ? MAX_LIST : parseInt(text);
  const bad = `Invalid key_count_limit of ${limit}.`;
  if (Number.isNaN(limit) || limit < 1) {
    throw new Fail(400, `${bad} Please specify an integer greater than 0.`);
  }
  if (limit > MAX_LIST) {
    throw new Fail(
      400,
      `${bad} Please specify an integer less than ${MAX_LIST}.`
    );
  }
  let after = '';
  const cursor = params.get('cursor');
  if (cursor) {
    try {
      after = utf8(Uint8Array.from(atob(cursor), (c) => c.charCodeAt(0)));
    } catch {
      throw new Fail(400, 'Invalid cursor.');
    }
  }
  const prefix = params.get('prefix') ?? '';
  validateLength(prefix);
  const rows = await space.shard.list(prefix, after, limit + 1);
  const keys = rows.slice(0, limit).map(({ key, expiration, metadata }) => {
    const k = { name: key };
    if (expiration !== null) k.expiration = Math.floor(expiration / 1000);
    if (metadata !== null) k.metadata = metadata;
    return k;
  });
  const page = { keys, list_complete: rows.length <= limit };
  if (!page.list_complete) {
    const bytes = new TextEncoder().encode(keys.at(-1).name);
    page.cursor = btoa(String.fromCharCode(...bytes));
  }
  return Response.json(page);
}

async function bulk(request, env, space) {
  let body;
  try {
    body = await request.json();
  } catch {
    throw new Fail(400, 'Invalid JSON');
  }
  if (typeof body !== 'object' || body === null) {
    throw new Fail(400, 'Invalid JSON');
  }
  const keys = Array.isArray(body.keys) ? body.keys : [];
  const type = body.type ?? 'text';
  if (type !== 'text' && type !== 'json') {
    throw new Fail(400, `"${type}" is not a valid type. Use "json" or "text"`);
  }
  if (keys.length > MAX_BULK) {
    throw new Fail(400, `You can request a maximum of ${MAX_BULK} keys`);
  }
  if (keys.length < 1) {
    throw new Fail(400, 'You must request a minimum of 1 key');
  }
  for (const key of keys) validate(String(key));
  const ttl = cacheTtl(body.cacheTtl);
  const got = await read(env, space, keys, ttl, MAX_BULK_BYTES);
  const out = {};
  keys.forEach((key, i) => {
    const { entry } = got[i];
    if (entry === null) {
      out[key] = null;
      return;
    }
    // Text decodes as a single get does; JSON must be valid UTF-8.
    let value = new TextDecoder().decode(entry.value);
    if (type === 'json') {
      try {
        value = JSON.parse(utf8(entry.value));
      } catch {
        throw new Fail(
          400,
          'At least one of the requested keys corresponds to a non-json value'
        );
      }
    }
    out[key] = body.withMetadata
      ? { value, metadata: entry.metadata && JSON.parse(entry.metadata) }
      : value;
  });
  return Response.json(out);
}

// =======================================================================
// Reads and the cache

const cache = new Map();
let cached = 0;

const cacheKey = (space, key) => `${space.ns}\0${key}`;

function forget(k) {
  const c = cache.get(k);
  if (c === undefined) return;
  cache.delete(k);
  cached -= c.size;
}

// What an entry costs: its strings at two bytes a character, its inline
// value, and about 128 bytes for the map entry and the objects.
function cost(k, row) {
  const chars =
    k.length + (row?.metadata?.length ?? 0) + (row?.blob?.length ?? 0);
  return 2 * chars + (row?.value?.byteLength ?? 0) + 128;
}

// `row` (null for a miss) for `ttl` seconds, as of the object's `version`,
// unless the cache holds a later one: replies to one node's concurrent
// reads and writes can arrive out of order.
function remember(space, key, row, ttl, version) {
  if (!clustered) return;
  const k = cacheKey(space, key);
  if ((cache.get(k)?.version ?? -1) > version) return;
  forget(k);
  const size = cost(k, row);
  const at = clock();
  cache.set(k, { row, at, until: at + ttl * 1000, size, version });
  cached += size;
  for (const old of cache.keys()) {
    if (cached <= CACHE_BYTES) break;
    forget(old);
  }
}

// The cached row, if no older than the reader's `ttl` seconds.
function recall(space, key, ttl) {
  const k = cacheKey(space, key);
  const c = cache.get(k);
  if (c === undefined) return undefined;
  const now = clock();
  const old = c.until <= now || now - c.at >= ttl * 1000;
  if (old || (c.row?.expiration ?? Infinity) <= now) {
    forget(k);
    return undefined;
  }
  cache.delete(k);
  cache.set(k, c);
  return c.row;
}

// Each key's entry (`{value, expiration, metadata}` or null) and whether
// its row came from the cache, refused with a 413 once their values pass
// `cap` bytes, before any further blob is read.
async function read(env, space, keys, ttl, cap = Infinity) {
  const rows = keys.map((key) => recall(space, key, ttl));
  const hits = rows.map((row) => row !== undefined);
  const misses = keys.filter((_, i) => !hits[i]);
  if (misses.length > 0) {
    const got = await space.shard.read(misses);
    let m = 0;
    keys.forEach((key, i) => {
      if (hits[i]) return;
      rows[i] = got.rows[m++];
      remember(space, key, rows[i], ttl, got.version);
    });
  }
  const out = [];
  let total = 0;
  for (const [i, key] of keys.entries()) {
    const entry = await load(env, space, key, rows[i], ttl);
    total += entry?.value.byteLength ?? 0;
    if (total > cap) {
      throw new Fail(
        413,
        `Total size of request exceeds the limit of ${cap / 1024 / 1024}MB`
      );
    }
    out.push({ entry, hit: hits[i] });
  }
  return out;
}

// A row as an entry, reading its blob. A blob that is gone was replaced
// since the row was read, so the row is read again.
async function load(env, space, key, row, ttl) {
  for (let i = 0; ; i++) {
    if (row === null) return null;
    const { expiration, metadata } = row;
    if (row.blob === null) {
      return { value: new Uint8Array(row.value), expiration, metadata };
    }
    const value = await readBlob(env, row.blob);
    if (value !== null) return { value, expiration, metadata };
    if (i === READS) throw new Error('Reading the value failed: 404.');
    const { version, rows } = await space.shard.read([key]);
    remember(space, key, (row = rows[0]), ttl, version);
  }
}

// =======================================================================
// Blobs, and their cache on a cluster node's disk

// The blobs in `BLOB_CACHE` by path, least recently used first, with
// their sizes. The Worker and its objects share the process's isolate, so
// this is the node's one index. It is in memory, so each process caches in
// a directory of its own, `c/<generation>/`, and deletes the others'.
const cachedBlobs = new Map();
let cachedBytes = 0;
let generation = null;

function cacheUrl(path) {
  return `http://cache/c/${generation}/${path}`;
}

// The cache, null if there is none. The first call deletes the other
// generations.
async function blobCache(env) {
  const cache = env.BLOB_CACHE;
  if (cache === undefined || generation !== null) return cache ?? null;
  generation = hex(crypto.getRandomValues(new Uint8Array(8)));
  const r = await cache.fetch('http://cache/c');
  for (const { name } of r.ok ? await r.json() : []) {
    if (name === generation) continue;
    await cache.fetch(`http://cache/c/${name}`, { method: 'DELETE' });
  }
  return cache;
}

// A blob's bytes, or null if it is not in `BLOBS`.
async function readBlob(env, path) {
  const cache = await blobCache(env);
  if (cache !== null && cachedBlobs.has(path)) {
    const r = await cache.fetch(cacheUrl(path));
    if (r.ok) {
      const size = cachedBlobs.get(path);
      cachedBlobs.delete(path);
      cachedBlobs.set(path, size);
      return new Uint8Array(await r.arrayBuffer());
    }
    await uncacheBlob(cache, path);
  }
  const r = await env.BLOBS.fetch(`http://blobs/${path}`);
  if (r.status === 404) return null;
  if (!r.ok) throw new Error(`Reading the value failed: ${r.status}.`);
  const value = new Uint8Array(await r.arrayBuffer());
  await cacheBlob(env, path, value);
  return value;
}

// Writes a blob to the cache, evicting the least recently used past
// `BLOB_CACHE_BYTES`.
async function cacheBlob(env, path, value) {
  const cache = await blobCache(env);
  const size = value.byteLength;
  if (cache === null || size > BLOB_CACHE_BYTES) return;
  const r = await cache.fetch(cacheUrl(path), { method: 'PUT', body: value });
  if (!r.ok || cachedBlobs.has(path)) return;
  cachedBlobs.set(path, size);
  cachedBytes += size;
  const evicted = [];
  for (const old of cachedBlobs.keys()) {
    if (cachedBytes <= BLOB_CACHE_BYTES) break;
    evicted.push(uncacheBlob(cache, old));
  }
  await Promise.all(evicted);
}

async function uncacheBlob(cache, path) {
  const size = cachedBlobs.get(path);
  if (size === undefined) return;
  cachedBlobs.delete(path);
  cachedBytes -= size;
  await cache.fetch(cacheUrl(path), { method: 'DELETE' });
}

const hex = (bytes) =>
  Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');

const utf8 = (bytes) => new TextDecoder('utf-8', { fatal: true }).decode(bytes);

// The least string above every string starting with `prefix`, or null if
// there is none.
function above(prefix) {
  const points = [...prefix];
  while (points.length > 0) {
    const last = points.pop().codePointAt(0);
    if (last < 0x10ffff) {
      const next = last === 0xd7ff ? 0xe000 : last + 1;
      return points.join('') + String.fromCodePoint(next);
    }
  }
  return null;
}

// =======================================================================
// The namespace's object

// The schema, as the steps from each version to the next: a database at
// version `v` (its `schema` row; 0 when new) runs `MIGRATIONS.slice(v)`.
const MIGRATIONS = [
  (sql) => {
    sql.exec(`CREATE TABLE entries (
      key TEXT PRIMARY KEY, value BLOB, blob TEXT, expiration INTEGER,
      metadata TEXT)`);
    sql.exec('CREATE INDEX entries_expiration ON entries(expiration)');
    // Blobs to delete when due: reserved ones, keyed by the key whose write
    // may claim them, and unclaimable ones, keyed by null.
    sql.exec(`CREATE TABLE garbage (
      blob TEXT PRIMARY KEY, due INTEGER NOT NULL, key TEXT)`);
    // The number of writes so far: each write's version, and a read's.
    sql.exec('CREATE TABLE clock (v INTEGER NOT NULL)');
    sql.exec('INSERT INTO clock VALUES (0)');
  },
];

export class KVNamespace extends DurableObject {
  #alarm = null;

  constructor(ctx, env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    // The version is read in the transaction that migrates, so two processes
    // opening a new namespace at once don't both migrate it.
    ctx.storage.transactionSync(() => {
      this.sql.exec('CREATE TABLE IF NOT EXISTS schema (v INTEGER NOT NULL)');
      const v = this.sql.exec('SELECT v FROM schema').toArray()[0]?.v ?? 0;
      if (v > MIGRATIONS.length) {
        throw new Error(
          `This namespace's schema (${v}) is newer than this workerd's ` +
            `(${MIGRATIONS.length}).`
        );
      }
      if (v === MIGRATIONS.length) return;
      for (const migrate of MIGRATIONS.slice(v)) migrate(this.sql);
      this.sql.exec('DELETE FROM schema');
      this.sql.exec('INSERT INTO schema VALUES (?)', MIGRATIONS.length);
    });
    ctx.blockConcurrencyWhile(async () => {
      this.#alarm = await ctx.storage.getAlarm();
    });
  }

  // Each key's row, or null if it is absent or expired, and the version
  // they are as of.
  read(keys) {
    const now = clock();
    const [{ v }] = this.sql.exec('SELECT v FROM clock').toArray();
    const rows = keys.map(
      (key) =>
        this.sql
          .exec(
            `SELECT value, blob, expiration, metadata FROM entries
             WHERE key = ? AND (expiration IS NULL OR expiration > ?)`,
            key,
            now
          )
          .toArray()[0] ?? null
    );
    return { version: v, rows };
  }

  // Reserves `blob` before it is written, so that it is deleted unless a
  // write of `key` claims it.
  reserve(key, blob) {
    this.#collect(blob, Date.now() + RESERVE_MS, key);
  }

  // The write's version.
  write(key, value, blob, expiration, metadata) {
    if (blob !== null) {
      const claimed = this.sql.exec(
        'DELETE FROM garbage WHERE blob = ? AND key IS ? RETURNING blob',
        blob,
        key
      );
      if (claimed.toArray().length === 0) {
        throw new Error('The value was collected before its write.');
      }
    }
    this.#drop(key);
    this.sql.exec(
      'INSERT INTO entries VALUES (?, ?, ?, ?, ?)',
      key,
      value,
      blob,
      expiration,
      metadata
    );
    if (expiration !== null) this.#wake(expiration);
    return this.#tick();
  }

  // The delete's version.
  remove(key) {
    this.#drop(key);
    return this.#tick();
  }

  #tick() {
    return this.sql.exec('UPDATE clock SET v = v + 1 RETURNING v').one().v;
  }

  #drop(key) {
    const removed = this.sql.exec(
      'DELETE FROM entries WHERE key = ? RETURNING blob',
      key
    );
    for (const { blob } of removed) {
      if (blob !== null) this.#collect(blob, Date.now() + this.#grace());
    }
  }

  // Up to `limit` unexpired keys starting with `prefix`, after `after`.
  list(prefix, after, limit) {
    const end = above(prefix);
    return this.sql
      .exec(
        `SELECT key, expiration, metadata FROM entries
         WHERE key > ? AND key >= ? AND (? IS NULL OR key < ?)
         AND substr(key, 1, length(?)) = ?
         AND (expiration IS NULL OR expiration > ?)
         ORDER BY key LIMIT ?`,
        after,
        prefix,
        end,
        end,
        prefix,
        prefix,
        clock(),
        limit
      )
      .toArray();
  }

  // Deletes expired rows and due blobs, then waits for the next.
  async alarm() {
    this.#alarm = null;
    const now = Date.now();
    const expired = this.sql.exec(
      'SELECT key FROM entries WHERE expiration <= ? LIMIT 1000',
      now
    );
    for (const { key } of expired.toArray()) this.#drop(key);
    // A due blob stays in `garbage` until its delete succeeds, unclaimable
    // and due again a grace period later, so a crash cannot leak it.
    const due = this.sql
      .exec('SELECT blob FROM garbage WHERE due <= ? LIMIT 100', now)
      .toArray();
    for (const { blob } of due) this.#collect(blob, now + this.#grace());
    await Promise.all(
      due.map(async ({ blob }) => {
        const r = await this.env.BLOBS.fetch(`http://blobs/${blob}`, {
          method: 'DELETE',
        });
        const cache = this.env.BLOB_CACHE;
        if (cache !== undefined && generation !== null) {
          await uncacheBlob(cache, blob);
        }
        if (r.ok || r.status === 404) {
          this.sql.exec(
            'DELETE FROM garbage WHERE blob = ? AND key IS NULL',
            blob
          );
        }
      })
    );
    const [{ next }] = this.sql
      .exec(
        `SELECT min(t) AS next FROM (
           SELECT min(due) AS t FROM garbage
           UNION ALL SELECT min(expiration) FROM entries)`
      )
      .toArray();
    if (next !== null) this.#wake(Math.max(next, Date.now() + 100));
  }

  #grace() {
    return this.env.TEST_CLOCK ? TEST_GRACE_MS : GRACE_MS;
  }

  #collect(blob, due, key = null) {
    this.sql.exec(
      'INSERT OR REPLACE INTO garbage VALUES (?, ?, ?)',
      blob,
      due,
      key
    );
    this.#wake(due);
  }

  #wake(at) {
    if (this.#alarm !== null && this.#alarm <= at) return;
    this.ctx.storage.setAlarm(at);
    this.#alarm = at;
  }
}
