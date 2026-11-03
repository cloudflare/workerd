// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Built-in KV, on the protocol workerd's KV binding speaks
// (src/workerd/api/kv.c++): GET, PUT and DELETE on /<key>, GET /?prefix=
// for a page of keys, POST /bulk/get. The binding's props name the
// namespace.
//
// A namespace is a Durable Object (`KVNamespace`, by `idFromName`) whose
// SQLite table `entries` is its index: each key's expiration, metadata,
// and its value inline up to `INLINE` bytes or else the path of a blob.
// Blobs live in the service `BLOBS` (GET, PUT and DELETE on
// /<dir>/b/<id>), a directory on disk or the cluster's bucket. A blob is
// immutable: it is reserved, written, then claimed by its row's write, and
// deleted after the row stops naming it, through the object's `garbage`
// table and its alarm.
//
// This Worker reads and writes blobs itself, so large values do not pass
// through the object, and keeps a per-process cache of rows and misses for
// each read's `cacheTtl`, which this process's writes update and nothing
// else invalidates. Rows name blobs rather than hold them, so entries are
// small; blobs never change, and in a cluster `BLOBS` caches them on the
// node's disk.

import { DurableObject } from 'cloudflare:workers';

const INLINE = 64 << 10;
const MAX_VALUE = 25 << 20;
const MAX_METADATA = 1024;
const MAX_LIST = 1000;
const MAX_BULK = 100;
const MIN_EXPIRATION_TTL = 60;
const MIN_CACHE_TTL = 30;
const CACHE_TTL = 60;
const CACHE_BYTES = 64 << 20;
// A reserved blob unclaimed this long is deleted.
const RESERVE_MS = 600_000;
// A blob no row names is deleted this much later, which spares readers
// that read the row a moment before a retry.
const GRACE_MS = 2_000;
const READS = 3;

class Fail extends Error {
  constructor(status, text) {
    super(text);
    this.status = status;
  }
}

const fail = (status, text) =>
  new Response(null, { status, statusText: text.replace(/[\r\n]/g, ' ') });

export default {
  async fetch(request, env, ctx) {
    try {
      return await serve(request, env, ctx.props.namespace);
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
  const key = decodeURIComponent(path.slice(1));
  switch (request.method) {
    case 'POST':
      if (path === '/bulk/get') return bulk(request, env, space);
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

async function put(request, env, space, key, params) {
  const value = new Uint8Array(await request.arrayBuffer());
  if (value.byteLength > MAX_VALUE) {
    throw new Fail(413, `Value length exceeds limit of ${MAX_VALUE}.`);
  }
  const expiration = expires(params);
  const metadata = request.headers.get('CF-KV-Metadata');
  if (metadata !== null && metadata.length > MAX_METADATA) {
    throw new Fail(
      413,
      `Metadata length of ${metadata.length} exceeds limit of ` +
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

// An `expiration` or `expiration_ttl` parameter as a time in ms.
function expires(params) {
  const now = Math.floor(Date.now() / 1000);
  const parse = (name) => {
    const v = params.get(name);
    if (v === null) return null;
    if (!/^\d+$/.test(v)) throw new Fail(400, `Invalid ${name} of ${v}.`);
    return Number(v);
  };
  const ttl = parse('expiration_ttl');
  if (ttl !== null) {
    if (ttl < MIN_EXPIRATION_TTL) {
      throw new Fail(
        400,
        `Invalid expiration_ttl of ${ttl}. Expiration TTL must be at ` +
          `least ${MIN_EXPIRATION_TTL}.`
      );
    }
    return (now + ttl) * 1000;
  }
  const at = parse('expiration');
  if (at !== null && at < now + MIN_EXPIRATION_TTL) {
    throw new Fail(
      400,
      `Invalid expiration of ${at}. Expiration times must be at least ` +
        `${MIN_EXPIRATION_TTL} seconds in the future.`
    );
  }
  return at === null ? null : at * 1000;
}

async function list(space, params) {
  const limit = params.has('key_count_limit')
    ? Number(params.get('key_count_limit'))
    : MAX_LIST;
  if (!Number.isInteger(limit) || limit < 1 || limit > MAX_LIST) {
    throw new Fail(
      400,
      `Invalid key_count_limit. Please specify an integer 1 to ${MAX_LIST}.`
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
  const keys = Array.isArray(body.keys) ? body.keys : [];
  if (keys.length < 1 || keys.length > MAX_BULK) {
    throw new Fail(400, `You must request 1 to ${MAX_BULK} keys`);
  }
  const type = body.type ?? 'text';
  if (type !== 'text' && type !== 'json') {
    throw new Fail(400, `"${type}" is not a valid type. Use "json" or "text"`);
  }
  const ttl = cacheTtl(body.cacheTtl);
  const got = await read(env, space, keys, ttl);
  const out = {};
  keys.forEach((key, i) => {
    const { entry } = got[i];
    if (entry === null) {
      out[key] = null;
      return;
    }
    let value = utf8(entry.value);
    if (type === 'json') {
      try {
        value = JSON.parse(value);
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
  const k = cacheKey(space, key);
  if ((cache.get(k)?.version ?? -1) > version) return;
  forget(k);
  const size = cost(k, row);
  cache.set(k, { row, until: Date.now() + ttl * 1000, size, version });
  cached += size;
  for (const old of cache.keys()) {
    if (cached <= CACHE_BYTES) break;
    forget(old);
  }
}

function recall(space, key) {
  const k = cacheKey(space, key);
  const c = cache.get(k);
  if (c === undefined) return undefined;
  const now = Date.now();
  if (c.until <= now || (c.row?.expiration ?? Infinity) <= now) {
    forget(k);
    return undefined;
  }
  cache.delete(k);
  cache.set(k, c);
  return c.row;
}

// Each key's entry (`{value, expiration, metadata}` or null) and whether
// its row came from the cache.
async function read(env, space, keys, ttl) {
  const rows = keys.map((key) => recall(space, key));
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
  const entries = await Promise.all(
    keys.map((key, i) => load(env, space, key, rows[i], ttl))
  );
  return entries.map((entry, i) => ({ entry, hit: hits[i] }));
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
    const r = await env.BLOBS.fetch(`http://blobs/${row.blob}`);
    if (r.ok) {
      const value = new Uint8Array(await r.arrayBuffer());
      return { value, expiration, metadata };
    }
    if (r.status !== 404 || i === READS) {
      throw new Error(`Reading the value failed: ${r.status}.`);
    }
    const { version, rows } = await space.shard.read([key]);
    remember(space, key, (row = rows[0]), ttl, version);
  }
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

export class KVNamespace extends DurableObject {
  #alarm = null;

  constructor(ctx, env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec(`CREATE TABLE IF NOT EXISTS entries (
      key TEXT PRIMARY KEY, value BLOB, blob TEXT, expiration INTEGER,
      metadata TEXT)`);
    this.sql.exec(`CREATE INDEX IF NOT EXISTS entries_expiration
      ON entries(expiration)`);
    // Blobs to delete when due: reserved ones, keyed by the key whose write
    // may claim them, and unclaimable ones, keyed by null.
    this.sql.exec(`CREATE TABLE IF NOT EXISTS garbage (
      blob TEXT PRIMARY KEY, due INTEGER NOT NULL, key TEXT)`);
    // The number of writes so far: each write's version, and a read's.
    this.sql.exec('CREATE TABLE IF NOT EXISTS clock (v INTEGER NOT NULL)');
    this.sql.exec(
      'INSERT INTO clock SELECT 0 WHERE NOT EXISTS (SELECT * FROM clock)'
    );
    ctx.blockConcurrencyWhile(async () => {
      this.#alarm = await ctx.storage.getAlarm();
    });
  }

  // Each key's row, or null if it is absent or expired, and the version
  // they are as of.
  read(keys) {
    const now = Date.now();
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
      if (blob !== null) this.#collect(blob, Date.now() + GRACE_MS);
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
        Date.now(),
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
    for (const { blob } of due) this.#collect(blob, now + GRACE_MS);
    await Promise.all(
      due.map(async ({ blob }) => {
        const r = await this.env.BLOBS.fetch(`http://blobs/${blob}`, {
          method: 'DELETE',
        });
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
