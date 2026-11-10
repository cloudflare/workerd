// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Queues, on the protocol workerd's `queue` binding speaks (POST /message
// and /batch, GET /metrics; src/workerd/api/queue.c++) and the `queue()`
// handler a service binding calls. The binding's props name the queue:
//
//   queue = (name = "<this service>",
//            props = (json = "{\"queue\": \"<name>\"}"))
//
// The config that runs this module (`bundled = "/workerd/queues.js"`) is an
// interface: cli/wrangler.rs and hand-written configs write it, each for the
// workerd it runs on. Anything added to it later must be optional. It is:
//
//   - a Worker with RPC and the `service_binding_extra_handlers` flag;
//   - the Durable Object class `QueueShard`, SQLite-backed (`enableSql`),
//     stored on a local disk. Its `uniqueKey` and that disk are where the
//     messages live: a config that changes either starts empty;
//   - these bindings:
//
//   SHARD      the `QueueShard` class
//   CONSUMERS  JSON: `{"<queue>": {"consumer": "<binding>", ...}}`, each
//              queue's consumer, a service binding to the Worker whose
//              `queue()` handler receives its batches, and its settings,
//              production's: `maxBatchSize` (10, at most 100),
//              `maxBatchTimeout` (5 s, at most 60), `maxRetries` (3),
//              `deadLetterQueue`, `retryDelay` (0 s) and `maxConcurrency`
//              (0, no limit). A queue need not be listed to be sent to.
//   CLUSTERED  JSON `true` in a cluster (optional)
//   TEST_CLOCK JSON `true` for tests (optional)
//
// A queue is up to 1024 shards, each a Durable Object (`<queue>#<n>`) whose
// messages are rows of a SQLite table, one each. A delivery leases its rows
// for `LEASE_MS` under a generation, and settling a row is fenced by it: an
// ack deletes the row, a retry makes it visible again later. A lease that
// expires, or that the last instance held, is a failed attempt. A message out
// of attempts goes to the dead letter queue in two phases, deleted once the
// target has it (at least once). Messages are kept for `RETENTION_MS` at
// most.
//
// A producer sends to a shard on its own instance: each isolate picks a shard
// by hashing a random token of its own with the queue, first touch places
// the shard here, and a shard answering from elsewhere moves the pick on for
// the next send. The shard delivers to the consumer on its own instance, so in
// a cluster sends and deliveries spread with the instances, and with threads
// once each thread has an isolate. A consumer's `maxConcurrency` limits the
// queue to that many shards, each with one batch in flight.
// Metrics, from `metrics()` and with each send or batch, are the shard's,
// not the queue's.
//
// Delivery runs on the object's alarm, set for the next batch deadline,
// lease expiry, dead letter send or retention sweep, so a survivor adopting a
// dead instance's shards delivers their messages. A full batch is delivered
// at once, without waiting for the alarm.
//
// With `TEST_CLOCK`, POST /clock?advance=<ms> moves this process's clock;
// a shard acts on it at its next call or alarm, and alarms keep the real
// clock.

import { DurableObject, env } from 'cloudflare:workers';

const SHARDS = 1024;
// Production's limits: 128 KB a message, 100 messages and 256 KB a batch.
const MAX_MESSAGE_BYTES = 128_000;
const MAX_BATCH_MESSAGES = 100;
const MAX_BATCH_BYTES = 256_000;
// Batches a shard has in flight without `maxConcurrency`.
const IN_FLIGHT = 8;
// A full shard refuses sends and its producers move to another.
const SHARD_BYTES = 256 * 1024 * 1024;
const SHARD_MESSAGES = 1_000_000;
const ATTEMPTS = 4;
const RETRY_MS = 10_000;
// A delivery outlasting its lease is taken to have failed: past a consumer
// invocation's 15 minutes.
const LEASE_MS = 16 * 60 * 1000;
// Production's default retention.
const RETENTION_MS = 4 * 24 * 60 * 60 * 1000;
const FORMATS = ['v8', 'text', 'json', 'bytes'];

// Each queue's consumer, with production's defaults: a consumer with no
// such service binding fails the config when it loads.
const consumers = Object.fromEntries(
  Object.entries(env.CONSUMERS ?? {}).map(([queue, c]) => {
    if (typeof env[c.consumer]?.queue !== 'function') {
      throw new Error(`queue "${queue}": no service binding "${c.consumer}"`);
    }
    return [
      queue,
      {
        deadLetterQueue: '',
        retryDelay: 0,
        maxConcurrency: 0,
        maxRetries: 3,
        ...c,
        maxBatchSize: Math.max(
          1,
          Math.min(c.maxBatchSize ?? 10, MAX_BATCH_MESSAGES)
        ),
        maxBatchTimeout: Math.min(c.maxBatchTimeout ?? 5, 60),
      },
    ];
  })
);

// This isolate's token: in a cluster random, made on first use (not allowed
// at load), else fixed, so a restart sends to the same shard.
// TODO(soon): one per thread, once an instance runs several.
let skew = 0;
const clock = () => Date.now() + skew;

let token;
const me = () => (token ??= env.CLUSTERED ? crypto.randomUUID() : 'local');
const picks = new Map();

function fnv(s) {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++)
    h = Math.imul(h ^ s.charCodeAt(i), 16777619);
  return h >>> 0;
}

// A shard of `queue` to send to, preferably on this instance.
function shard(queue) {
  const p = picks.get(queue) ?? { miss: 0 };
  picks.set(queue, p);
  const n = consumers[queue]?.maxConcurrency || SHARDS;
  const name = `${queue}#${fnv(`${me()}/${queue}/${p.miss}`) % n}`;
  return { stub: env.SHARD.getByName(name), miss: () => p.miss++ };
}

// Calls a shard of `queue` by RPC: `op` is the producer protocol's path. A
// full shard moves the pick on and the call to the next.
async function send(queue, op, format, delay, body) {
  for (let i = 0; ; i++) {
    const { stub, miss } = shard(queue);
    const r = await stub.call(queue, op, format, delay, body);
    if (r.instance !== me() || r.status === 429) miss();
    if (r.status !== 429 || i + 1 === ATTEMPTS) return r;
  }
}

export default {
  async fetch(request, _env, ctx) {
    const url = new URL(request.url);
    const { pathname } = url;
    if (env.TEST_CLOCK && pathname === '/clock') {
      skew += Number(url.searchParams.get('advance'));
      return Response.json({ now: clock() });
    }
    if (pathname === '/dead') return new Response('Not Found', { status: 404 });
    const body = request.method === 'POST' ? await request.arrayBuffer() : null;
    const format = request.headers.get('X-Msg-Fmt') ?? 'v8';
    const delay = Number(request.headers.get('X-Msg-Delay-Secs') ?? 0);
    const r = await send(ctx.props.queue, pathname, format, delay, body);
    return new Response(r.body, { status: r.status });
  },
};

const decoder = new TextDecoder();

// A shard's messages, one row each. A row not leased is visible from
// `visible_at`; a leased one is out with a consumer until `leased_until`,
// under the generation `lease`; `dead` marks one whose attempts are spent,
// on its way to the dead letter queue.
const SCHEMA = `
CREATE TABLE IF NOT EXISTS messages (
  seq INTEGER PRIMARY KEY,
  id TEXT NOT NULL,
  format INTEGER NOT NULL,
  body BLOB NOT NULL,
  size INTEGER NOT NULL,
  timestamp REAL NOT NULL,
  visible_at REAL NOT NULL,
  attempts INTEGER NOT NULL DEFAULT 0,
  dead INTEGER NOT NULL DEFAULT 0,
  lease INTEGER,
  leased_until REAL
);
CREATE INDEX IF NOT EXISTS visible ON messages (visible_at, seq)
  WHERE lease IS NULL AND dead = 0;
CREATE INDEX IF NOT EXISTS buried ON messages (visible_at)
  WHERE lease IS NULL AND dead = 1;
CREATE INDEX IF NOT EXISTS leased ON messages (lease)
  WHERE lease IS NOT NULL;
CREATE INDEX IF NOT EXISTS retried ON messages (visible_at)
  WHERE lease IS NULL AND dead = 0 AND attempts > 0;
CREATE INDEX IF NOT EXISTS age ON messages (timestamp);
`;

const COLUMNS = 'seq, id, format, body, timestamp, attempts, visible_at';
const VISIBLE = 'lease IS NULL AND dead = 0';
// A retried message waits for its delay, not for the batch timeout again.
const RETRIED = `SELECT min(visible_at) AS t FROM messages
  WHERE ${VISIBLE} AND attempts > 0`;
const inOrder = (a, b) => a.visible_at - b.visible_at || a.seq - b.seq;

export class QueueShard extends DurableObject {
  #sql;
  #count = 0;
  #bytes = 0;
  #generation = 0;
  // Each delivery's generation and when its lease ends: one past it no
  // longer counts against the limit.
  #leases = new Map();
  // When the dead letter send out ends, 0 with none.
  #burying = 0;
  #alarm = null;
  #name;

  constructor(ctx, env) {
    super(ctx, env);
    const sql = (this.#sql = ctx.storage.sql);
    sql.exec(SCHEMA);
    // No lease outlives the instance that took it: what the last one had
    // out counts as a failed attempt.
    this.#fail(clock(), 'lease IS NOT NULL');
    const t = sql
      .exec('SELECT count(*) AS n, coalesce(sum(size), 0) AS b FROM messages')
      .one();
    this.#count = t.n;
    this.#bytes = t.b;
  }

  get #queue() {
    return (this.#name ??= this.ctx.storage.kv.get('queue'));
  }

  get #consumer() {
    return consumers[this.#queue];
  }

  // A producer's request, `op` its path, answered as the status and body
  // the protocol's response would have.
  call(queue, op, format, delay, body) {
    if (this.#queue === undefined) this.ctx.storage.kv.put('queue', queue);
    const reply = (status, res) => ({
      status,
      body: JSON.stringify(res),
      instance: me(),
    });
    const full = this.#count >= SHARD_MESSAGES || this.#bytes >= SHARD_BYTES;
    if (full && op !== '/metrics') return reply(429, 'Queue is overloaded.');
    if (op === '/message') {
      if (body.byteLength > MAX_MESSAGE_BYTES) {
        return reply(
          413,
          `message length of ${body.byteLength} bytes exceeds limit`
        );
      }
      this.#insert({ format, delay }, body);
    } else if (op === '/batch' || op === '/dead') {
      // A dead letter batch comes from a shard by RPC, its bodies as bytes.
      const dead = op === '/dead';
      const messages = dead ? body : JSON.parse(decoder.decode(body)).messages;
      const bodies = messages.map((m) =>
        dead ? m.body : Uint8Array.from(atob(m.body), (c) => c.charCodeAt(0))
      );
      const total = bodies.reduce((n, b) => n + b.byteLength, 0);
      const largest = Math.max(0, ...bodies.map((b) => b.byteLength));
      // A dead letter batch is a delivered batch, past the producer limits.
      const error = dead
        ? null
        : messages.length > MAX_BATCH_MESSAGES
          ? `batch message count of ${messages.length}`
          : largest > MAX_MESSAGE_BYTES
            ? `message in batch of ${largest} bytes`
            : total > MAX_BATCH_BYTES
              ? `batch size of ${total} bytes`
              : null;
      if (error) return reply(413, `${error} exceeds limit`);
      messages.forEach((m, i) =>
        this.#insert(
          { ...m, format: m.contentType ?? 'v8', delay: m.delaySecs ?? delay },
          bodies[i]
        )
      );
    } else if (op !== '/metrics') {
      return reply(404, 'Not Found');
    }
    this.#pump();
    const metrics = this.#metrics();
    return reply(200, op === '/metrics' ? metrics : { metadata: { metrics } });
  }

  async alarm() {
    this.#alarm = null;
    this.#pump();
  }

  #insert(m, body) {
    const now = clock();
    this.#sql.exec(
      'INSERT INTO messages (id, format, body, size, timestamp, visible_at) ' +
        'VALUES (?, ?, ?, ?, ?, ?)',
      m.id ?? crypto.randomUUID().replaceAll('-', ''),
      FORMATS.indexOf(m.format),
      body,
      body.byteLength,
      m.timestamp ?? now,
      now + (m.delay ?? 0) * 1000
    );
    this.#count++;
    this.#bytes += body.byteLength;
  }

  #delete(where, ...args) {
    const q = `DELETE FROM messages WHERE ${where} RETURNING size`;
    for (const { size } of this.#sql.exec(q, ...args)) {
      this.#count--;
      this.#bytes -= size;
    }
  }

  // Ends the leases `where` selects as failed attempts, visible again `at`.
  // A message out of attempts is dead: without a dead letter queue, deleted.
  #fail(at, where, ...args) {
    const c = this.#consumer;
    this.#sql.exec(
      'UPDATE messages SET lease = NULL, leased_until = NULL, ' +
        'attempts = attempts + 1, dead = dead OR attempts + 1 > ?, ' +
        `visible_at = ? WHERE ${where}`,
      c?.maxRetries ?? Number.MAX_SAFE_INTEGER,
      at,
      ...args
    );
    if (c && !c.deadLetterQueue) this.#delete('lease IS NULL AND dead = 1');
  }

  // Leases the rows `where` selects under a new generation, returning them
  // in delivery order.
  #lease(where, ...args) {
    const generation = ++this.#generation;
    const until = clock() + LEASE_MS;
    const rows = this.#sql
      .exec(
        'UPDATE messages SET lease = ?, leased_until = ? ' +
          `WHERE seq IN (SELECT seq FROM messages WHERE ${where}) ` +
          `RETURNING ${COLUMNS}`,
        generation,
        until,
        ...args
      )
      .toArray();
    return { generation, until, rows: rows.sort(inOrder) };
  }

  // Sweeps, delivers what is due and sets the alarm for what is next.
  #pump() {
    const now = clock();
    const sql = this.#sql;
    this.#delete('timestamp <= ?', now - RETENTION_MS);
    const c = this.#consumer;
    if (c === undefined) return this.#arm(now, null);
    this.#fail(now, 'lease IS NOT NULL AND leased_until <= ?', now);
    for (const [g, until] of this.#leases) {
      if (until <= now) this.#leases.delete(g);
    }
    this.#bury(now, c);
    const limit = c.maxConcurrency ? 1 : IN_FLIGHT;
    const ready =
      `${VISIBLE} AND visible_at <= ? ` + 'ORDER BY visible_at, seq LIMIT ?';
    while (this.#leases.size < limit) {
      const head = sql
        .exec(
          `SELECT visible_at FROM messages WHERE ${ready}`,
          now,
          c.maxBatchSize
        )
        .toArray();
      if (head.length === 0) break;
      const due = Math.min(
        head[0].visible_at + c.maxBatchTimeout * 1000,
        this.#min(RETRIED)
      );
      if (head.length < c.maxBatchSize && due > now) break;
      const lease = this.#lease(ready, now, c.maxBatchSize);
      const { generation } = lease;
      this.#leases.set(generation, lease.until);
      const delivery = this.#deliver(c, generation, lease.rows).catch((e) => {
        console.error(`queue "${this.#queue}": delivery failed: ${e}`);
        this.#fail(clock(), 'lease = ?', generation);
      });
      this.ctx.waitUntil(
        delivery.finally(() => {
          this.#leases.delete(generation);
          this.#pump();
        })
      );
    }
    this.#arm(now, c);
  }

  // Sets the alarm to the earliest of the batch deadline, a lease's expiry,
  // a dead letter's next send and the next retention sweep.
  #arm(now, c) {
    const min = (q) => this.#min(q);
    let next = min('SELECT min(timestamp) AS t FROM messages') + RETENTION_MS;
    if (c !== null) {
      // At the limit, the next delivery starts as one finishes.
      if (this.#leases.size < (c.maxConcurrency ? 1 : IN_FLIGHT)) {
        const visible = min(
          `SELECT min(visible_at) AS t FROM messages WHERE ${VISIBLE}`
        );
        const due = visible + c.maxBatchTimeout * 1000;
        next = Math.min(next, due, min(RETRIED));
      }
      next = Math.min(
        next,
        min(
          'SELECT min(leased_until) AS t FROM messages WHERE lease IS NOT NULL'
        )
      );
      if (c.deadLetterQueue && this.#burying <= now) {
        next = Math.min(
          next,
          min(
            'SELECT min(visible_at) AS t FROM messages ' +
              'WHERE lease IS NULL AND dead = 1'
          )
        );
      }
    }
    // A stale alarm costs one pump.
    if (next === Infinity) return;
    if (this.#alarm === null || next < this.#alarm || this.#alarm <= now) {
      this.ctx.storage.setAlarm(next - skew);
      this.#alarm = next;
    }
  }

  // Sends spent messages to the dead letter queue in two phases: leased,
  // sent, then deleted once it has them. A crash between sends them again.
  #bury(now, c) {
    if (this.#burying > now || !c.deadLetterQueue) return;
    const { generation, until, rows } = this.#lease(
      'lease IS NULL AND dead = 1 AND visible_at <= ? LIMIT ?',
      now,
      MAX_BATCH_MESSAGES
    );
    if (rows.length === 0) return;
    this.#burying = until;
    const letters = rows.map((m) => ({
      id: m.id,
      timestamp: m.timestamp,
      contentType: FORMATS[m.format],
      body: m.body,
    }));
    const done = send(c.deadLetterQueue, '/dead', 'v8', 0, letters)
      .then((r) => {
        if (r.status !== 200) throw new Error(`status ${r.status}`);
        this.#delete('lease = ?', generation);
      })
      .catch((e) => {
        console.error(`queue "${this.#queue}": dead lettering failed: ${e}`);
        this.#sql.exec(
          'UPDATE messages SET lease = NULL, leased_until = NULL, ' +
            'visible_at = ? WHERE lease = ?',
          clock() + RETRY_MS,
          generation
        );
      })
      .finally(() => {
        if (this.#burying === until) this.#burying = 0;
        this.#pump();
      });
    this.ctx.waitUntil(done);
  }

  #min(q) {
    return this.#sql.exec(q).one().t ?? Infinity;
  }

  #metrics() {
    const [first] = this.#sql
      .exec('SELECT timestamp FROM messages ORDER BY seq LIMIT 1')
      .toArray();
    return {
      backlogCount: this.#count,
      backlogBytes: this.#bytes,
      oldestMessageTimestamp: first?.timestamp ?? 0,
    };
  }

  async #deliver(c, generation, rows) {
    const messages = rows.map((m) => {
      const msg = {
        id: m.id,
        timestamp: new Date(m.timestamp),
        attempts: m.attempts + 1,
      };
      const format = FORMATS[m.format];
      if (format === 'text') msg.body = decoder.decode(m.body);
      else if (format === 'json') msg.body = JSON.parse(decoder.decode(m.body));
      else if (format === 'bytes') msg.body = m.body;
      else msg.serializedBody = new Uint8Array(m.body);
      return msg;
    });
    let r;
    try {
      const metadata = { metrics: this.#metrics() };
      r = await this.env[c.consumer].queue(this.#queue, messages, metadata);
    } catch (e) {
      console.error(`queue "${this.#queue}": delivery failed: ${e}`);
      r = {
        outcome: 'exception',
        retryBatch: { retry: false },
        explicitAcks: [],
      };
    }
    const acks = new Set(r.explicitAcks);
    const retries = new Map(
      (r.retryMessages ?? []).map((m) => [m.msgId, m.delaySeconds])
    );
    const retryAll = r.retryBatch.retry || r.outcome !== 'ok';
    const now = clock();
    // Only rows still under this lease: one that expired is another
    // delivery's now.
    for (const m of rows) {
      const retry =
        !acks.has(m.id) && (retries.has(m.id) || (retryAll && !r.ackAll));
      if (!retry) {
        this.#delete('seq = ? AND lease = ?', m.seq, generation);
        continue;
      }
      const delay =
        retries.get(m.id) ?? r.retryBatch.delaySeconds ?? c.retryDelay;
      const at = now + delay * 1000;
      this.#fail(at, 'seq = ? AND lease = ?', m.seq, generation);
    }
  }
}
