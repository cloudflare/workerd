// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The Worker of cluster-threads.wd-test. Its top-level fetch() sends a request
// to the Counter named by the `name` query parameter, with the same path,
// query, method, headers and body, and answers what the Counter answers, or
// `{ error }` with status 500 if the call threw. It answers these itself:
//   /thread          This isolate's identity (after `ms` milliseconds, if
//                    given), which tells the threads of an instance apart.
//   /rpc-where       The Counter's where(), over JS RPC.
//   /rpc-increment   The Counter's increment(marker), over JS RPC.
//   /bucket/<key>    The request, method and body, to the `blobs` service for
//                    `<key>`, and its response, with `x-isolate`.
//   /cache/<key>     PUT stores the body in `caches.default`, GET matches it;
//                    the response has `x-isolate`.
// A request whose `x-thread` header names another isolate is refused
// (`threadRequest`).
//
// Each Counter reports `isolate`, the identity of the isolate it runs in,
// which is its home thread's copy of this Worker, and `nodeId`, the instance's
// NODE_ID. Its state is in SQLite: `marks` counts the increments of each
// marker, so that an increment that runs twice shows, and `rows` holds what
// /write wrote. Its paths:
//   /where           isolate, nodeId, id, and `instance`, new for every time
//                    the object is constructed.
//   /increment?marker=M
//                    Counts an increment of marker M (a new one if not given);
//                    answers `count`, the sum of all increments.
//   /hold-increment?ms=N&marker=M
//                    Waits N ms (default 1000), then increments.
//   /holding         How many /hold-increment requests are waiting.
//   /write?key=K&value=V
//                    Stores row K.
//   /read            The rows, the marks, and the count.
//   /echo            Answers the request's body, with `x-isolate`.
//   /ws              A hibernatable WebSocket, which answers each message M
//                    with `{ isolate, nodeId, count }` once it counted an
//                    increment of marker M.
//   /spin?ms=N       Keeps its thread in JavaScript for N ms, or until the
//                    epoch time `until`, then answers when it began and ended.
//   /wedge           Keeps its thread in JavaScript for good.
//   /set-alarm?ms=N  Sets the alarm N ms from now, or for the epoch time `at`.
//   /alarms          Each run of the alarm, `{ nodeId, isolate }`, in order.
//   /chain?targets=T Waits `ms` (default 0), sends /chain with the rest of the
//                    names T lists, separated by commas, to the first, which
//                    waits `next` ms (default 0), then increments: a call
//                    through other objects, or itself, back to it.

import { DurableObject } from 'cloudflare:workers';

// The identity of this isolate: global state, so each loop thread's copy of
// the Worker has its own.
let thread;

function isolate() {
  thread ??= crypto.randomUUID();
  return thread;
}

// The `x-attempt` of every request this isolate acted on.
const attempts = new Set();

// Answers `/thread` with this isolate's identity, and refuses a request whose
// `x-thread` header names another isolate with 409 and `refused`, before
// anything else is done with it (its body is read and dropped), for the client
// to send it again until that isolate serves it; nothing for any other
// request. The refusal is held for a while, so that the client's other
// attempts at once go to other threads. The attempts of one request carry one
// `x-attempt`, and the isolate named acts on the first it gets only.
async function threadRequest(request, url) {
  const wanted = request.headers.get('x-thread');
  const attempt = request.headers.get('x-attempt');
  if (
    wanted !== null &&
    (wanted !== isolate() || (attempt !== null && attempts.has(attempt)))
  ) {
    await request.arrayBuffer();
    await scheduler.wait(100);
    return Response.json({ thread: isolate(), refused: true }, { status: 409 });
  }
  if (attempt !== null) attempts.add(attempt);
  if (url.pathname !== '/thread') return undefined;
  await scheduler.wait(Number(url.searchParams.get('ms') ?? 0));
  return Response.json({ thread: isolate() });
}

function failure(err) {
  return Response.json(
    { error: String(err), retryable: err.retryable === true },
    { status: 500 }
  );
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const answer = await threadRequest(request, url);
    if (answer !== undefined) return answer;

    if (url.pathname.startsWith('/cache/')) {
      const key = `https://cache.test/${url.pathname.slice('/cache/'.length)}`;
      const headers = { 'x-isolate': isolate() };
      if (request.method === 'PUT') {
        await caches.default.put(
          key,
          new Response(request.body, {
            headers: { 'cache-control': 'max-age=3600' },
          })
        );
        return new Response(null, { status: 204, headers });
      }
      const match = await caches.default.match(key);
      if (match === undefined)
        return new Response(null, { status: 404, headers });
      return new Response(match.body, { headers });
    }

    if (url.pathname.startsWith('/bucket/')) {
      const key = url.pathname.slice('/bucket/'.length);
      const response = await env.BLOBS.fetch(`http://bucket/${key}`, {
        method: request.method,
        body: request.body,
      });
      const headers = new Headers(response.headers);
      headers.set('x-isolate', isolate());
      return new Response(response.body, {
        status: response.status,
        headers,
      });
    }

    const name = url.searchParams.get('name') ?? 'default';
    const stub = env.COUNTER.get(env.COUNTER.idFromName(name));
    try {
      if (url.pathname === '/rpc-where') {
        return Response.json(await stub.where());
      }
      if (url.pathname === '/rpc-increment') {
        return Response.json(
          await stub.increment(url.searchParams.get('marker'))
        );
      }
      return await stub.fetch(
        new Request(`http://do${url.pathname}${url.search}`, request)
      );
    } catch (err) {
      return failure(err);
    }
  },
};

export class Counter extends DurableObject {
  #instance = crypto.randomUUID();
  #holding = 0;

  constructor(ctx, env) {
    super(ctx, env);
    const sql = ctx.storage.sql;
    sql.exec(
      'CREATE TABLE IF NOT EXISTS marks (marker TEXT PRIMARY KEY, n INTEGER NOT NULL)'
    );
    sql.exec('CREATE TABLE IF NOT EXISTS rows (k TEXT PRIMARY KEY, v TEXT)');
    sql.exec(
      'CREATE TABLE IF NOT EXISTS fired (seq INTEGER PRIMARY KEY, node TEXT, isolate TEXT)'
    );
  }

  where() {
    return {
      isolate: isolate(),
      nodeId: this.env.NODE_ID,
      id: this.ctx.id.toString(),
      instance: this.#instance,
    };
  }

  #count() {
    return this.ctx.storage.sql
      .exec('SELECT COALESCE(SUM(n), 0) AS count FROM marks')
      .one().count;
  }

  increment(marker) {
    this.ctx.storage.sql.exec(
      'INSERT INTO marks (marker, n) VALUES (?, 1) ON CONFLICT (marker) DO UPDATE SET n = n + 1',
      marker ?? crypto.randomUUID()
    );
    return { ...this.where(), count: this.#count() };
  }

  async fetch(request) {
    const url = new URL(request.url);
    const query = url.searchParams;
    switch (url.pathname) {
      case '/where':
        return Response.json(this.where());
      case '/increment':
        return Response.json(this.increment(query.get('marker')));
      case '/hold-increment': {
        this.#holding++;
        try {
          await scheduler.wait(Number(query.get('ms') ?? 1000));
        } finally {
          this.#holding--;
        }
        return Response.json(this.increment(query.get('marker')));
      }
      case '/holding':
        return Response.json({ ...this.where(), holding: this.#holding });
      case '/write':
        this.ctx.storage.sql.exec(
          'INSERT OR REPLACE INTO rows (k, v) VALUES (?, ?)',
          query.get('key'),
          query.get('value')
        );
        return Response.json(this.where());
      case '/read': {
        const sql = this.ctx.storage.sql;
        const rows = Object.fromEntries(
          sql
            .exec('SELECT k, v FROM rows')
            .toArray()
            .map((r) => [r.k, r.v])
        );
        const marks = Object.fromEntries(
          sql
            .exec('SELECT marker, n FROM marks')
            .toArray()
            .map((r) => [r.marker, r.n])
        );
        return Response.json({
          ...this.where(),
          rows,
          marks,
          count: this.#count(),
        });
      }
      case '/echo': {
        const body = await request.arrayBuffer();
        return new Response(body, {
          headers: { 'x-isolate': isolate(), 'x-node': this.env.NODE_ID },
        });
      }
      case '/ws': {
        const pair = new WebSocketPair();
        this.ctx.acceptWebSocket(pair[1]);
        return new Response(null, { status: 101, webSocket: pair[0] });
      }
      case '/spin': {
        const start = Date.now();
        const stop = query.has('until')
          ? Number(query.get('until'))
          : start + Number(query.get('ms'));
        while (Date.now() < stop) {}
        return Response.json({ ...this.where(), start, end: Date.now() });
      }
      case '/wedge': {
        let sum = 0;
        for (;;) {
          sum = (sum * 31 + 1) | 0;
        }
      }
      case '/set-alarm':
        await this.ctx.storage.setAlarm(
          query.has('at')
            ? Number(query.get('at'))
            : Date.now() + Number(query.get('ms') ?? 60_000)
        );
        return Response.json(this.where());
      case '/alarms': {
        const fired = this.ctx.storage.sql
          .exec('SELECT node AS nodeId, isolate FROM fired ORDER BY seq')
          .toArray()
          .map((r) => ({ nodeId: r.nodeId, isolate: r.isolate }));
        return Response.json({ ...this.where(), fired });
      }
      case '/chain': {
        await scheduler.wait(Number(query.get('ms') ?? 0));
        const [next, ...rest] = (query.get('targets') ?? '')
          .split(',')
          .filter((t) => t !== '');
        if (next !== undefined) {
          const stub = this.env.COUNTER.get(this.env.COUNTER.idFromName(next));
          const response = await stub.fetch(
            `http://do/chain?ms=${query.get('next') ?? 0}&targets=${rest.join(',')}`
          );
          if (!response.ok) return response;
          await response.arrayBuffer();
        }
        return Response.json(this.increment(null));
      }
    }
    return new Response('not found', { status: 404 });
  }

  webSocketMessage(ws, message) {
    ws.send(JSON.stringify(this.increment(String(message))));
  }

  webSocketClose(ws) {
    ws.close(1000, 'closed');
  }

  alarm() {
    this.ctx.storage.sql.exec(
      'INSERT INTO fired (node, isolate) VALUES (?, ?)',
      this.env.NODE_ID,
      isolate()
    );
  }
}
