// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Cluster mode on several loop threads (`--threads`). An instance has one
// control plane, on its first thread, and a data plane on every thread for the
// objects whose home it is, so what the cluster does with an object (hand it
// off, take it over, refuse it, adopt its alarm, call it from a peer) is
// checked here for objects whose home is not the first thread.
//
// A test cannot ask where an object will live. It learns the threads of an
// instance by the isolates of their copies of the Worker (`layout`): the first
// thread's is the one behind the datagram socket, which only the first thread
// serves; the others answer `/thread` to requests the front gives them. It
// then asks objects where they run (`homes`) until it has objects on the
// threads it needs. Every case of Durable Objects fails if no object lives on
// a thread other than the first.
//
// Two instances with the same number of threads place an object on threads
// of the same rank (a hash of its namespace and key over the threads), which
// `sameRanks` checks wherever a test sees an object on both.

import dgram from 'node:dgram';
import { once } from 'node:events';
import { randomBytes } from 'node:crypto';
import { mkdir, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { env } from 'node:process';
import { test as nodeTest } from 'node:test';
import assert from 'node:assert';
import { setTimeout as sleep } from 'node:timers/promises';
import {
  fetchJson,
  nodeThreads,
  until,
  withCluster,
} from './cluster-harness.mjs';

assert(
  env.WORKERD_BINARY !== undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert(
  env.WD_TEST_CONFIG !== undefined,
  'You must set the WD_TEST_CONFIG environment variable.'
);

// Bazel shards the tests (`shard_count`): each shard runs every n-th of them.
// CLUSTER_TESTS, a regular expression, picks the tests to run.
const picked = new RegExp(env.CLUSTER_TESTS ?? '');
const SHARDS = Number(env.TEST_TOTAL_SHARDS ?? 1);
const SHARD = Number(env.TEST_SHARD_INDEX ?? 0);
if (env.TEST_SHARD_STATUS_FILE !== undefined) {
  await writeFile(env.TEST_SHARD_STATUS_FILE, '');
}
let testIndex = 0;
function test(name, ...rest) {
  if (picked.test(name) && testIndex++ % SHARDS === SHARD) {
    nodeTest(name, ...rest);
  }
}

// Node's SIGTERM is TerminateProcess on Windows, so an instance cannot drain
// there, and there is no SIGSTOP.
const UNIX_SIGNALS = {
  skip: process.platform === 'win32' && 'no SIGTERM drain or SIGSTOP',
};

// How many loop threads an instance with threads serves on.
const THREADS = 3;

// cluster-threads.wd-test divides the cluster's times by this
// (`unsafeTimeDivisor`), and `shortLease`'s by `SHORT_LEASE_DIVISOR`.
const TIME_DIVISOR = 4;
const SHORT_LEASE_DIVISOR = 10;
// How long an instance's lease lasts: a dead owner's objects wait this long.
const LEASE_MS = 20_000 / TIME_DIVISOR;
// How long an instance's loop may make no progress before it fences
// (`Timing::stall_ms`, cluster/logic/lease.rs), in `shortLease`.
const SHORT_LEASE_STALL_MS = 60_000 / SHORT_LEASE_DIVISOR;
// The stop timeout of `config`, and how it is spent (cluster/logic/handoff.rs):
// work in flight on an object being handed off is cancelled at half of it.
const STOP_MS = 10_000;
const IN_FLIGHT_MS = STOP_MS / 2;
// The stop timeout of `shortStop`.
const SHORT_STOP_MS = 8000;

// The Durable Object namespace of the config.
const UNIQUE_KEY = 'cluster-threads-counter';

// What makes the instances of `withCluster` serve on `counts[i]` threads, of
// the config's constant `configName`, each with a directory of its own for its
// `cache` service.
function cluster(counts, configName = 'config') {
  return {
    configName,
    extraArgs: async (i, root) => {
      const cache = join(root, `cache${i}`);
      await mkdir(cache);
      return [
        `--directory-path=cache=${cache}`,
        ...(counts[i] > 1 ? [`--threads=${counts[i]}`] : []),
      ];
    },
  };
}

// The isolate of the first thread of `node`: the datagram socket's Worker
// answers with it.
async function firstThread(node) {
  const port = await node.port('udp');
  const socket = dgram.createSocket('udp4');
  try {
    const greeting = once(socket, 'message');
    for (let tries = 0; ; tries++) {
      socket.send('hello', port, '127.0.0.1');
      const answered = await Promise.race([greeting, sleep(500)]);
      if (answered !== undefined) {
        return JSON.parse(answered[0].toString()).thread;
      }
      assert(
        tries < 20,
        `the datagram socket of ${node.nodeId} did not answer`
      );
    }
  } finally {
    socket.close();
  }
}

// The threads of `node`, which serves on `count`: `first`, the isolate of its
// first thread, `all` and `others` (all but the first) by isolate, and
// `threads`, through which a request reaches a thread of its choice.
async function layout(node, count) {
  const first = await firstThread(node);
  const threads = await nodeThreads(node, count);
  const all = threads.map((t) => t.thread);
  assert(all.includes(first), `${first} is not a thread of ${node.nodeId}`);
  const others = all.filter((t) => t !== first);
  return { node, first, all, others, threads };
}

// The JSON body of a GET of `path` from `node`, which must answer 200.
async function get(node, path, timeoutMs = 20_000) {
  const { status, body } = await fetchJson(node.httpPort, path, { timeoutMs });
  assert.strictEqual(
    status,
    200,
    `${path} on ${node.nodeId}: ${JSON.stringify(body)}`
  );
  return body;
}

// As `get`, answered once `node` answers 200, within `ms`: a takeover waits
// out the dead owner's lease.
async function eventually(node, path, ms = 30_000) {
  const deadline = Date.now() + ms;
  for (;;) {
    const answer = await fetchJson(node.httpPort, path, {
      timeoutMs: 10_000,
    }).catch((error) => ({ status: 0, body: { error: String(error) } }));
    if (answer.status === 200) return answer.body;
    assert(
      Date.now() < deadline,
      `${path} on ${node.nodeId}: ${JSON.stringify(answer.body)}`
    );
    await sleep(200);
  }
}

// Names of objects that `at.node` claims, by home: `per` on each of its
// threads, or of the threads `on`. Objects named `${prefix}-${i}` are asked
// where they run until each thread has enough.
async function homes(at, prefix, per = 1, on = at.all) {
  const byHome = new Map(on.map((thread) => [thread, []]));
  const short = () => [...byHome.values()].some((names) => names.length < per);
  const BATCH = 8;
  for (let i = 0; i < 64 * per && short(); i += BATCH) {
    const names = Array.from({ length: BATCH }, (_, j) => `${prefix}-${i + j}`);
    const wheres = await Promise.all(
      names.map((name) => get(at.node, `/where?name=${name}`))
    );
    for (const [j, where] of wheres.entries()) {
      assert.strictEqual(where.nodeId, at.node.nodeId, names[j]);
      assert(
        at.all.includes(where.isolate),
        `${names[j]} runs in ${where.isolate}`
      );
      const here = byHome.get(where.isolate);
      if (here !== undefined && here.length < per) here.push(names[j]);
    }
  }
  for (const [home, names] of byHome) {
    const rank = home === at.first ? 'the first thread' : `thread ${home}`;
    assert(
      names.length >= per,
      `${names.length} of the objects ${at.node.nodeId} claimed live on ${rank}`
    );
  }
  return byHome;
}

// The home of `name` in `byHome`.
function homeOf(byHome, name) {
  for (const [home, names] of byHome) if (names.includes(name)) return home;
  assert.fail(`${name} has no home`);
}

// Checks that each object of `seen`, `[name, home in one instance, home in
// the other]`, lives on threads of the same rank in both: objects that share
// a home in one share it in the other, and a first thread's objects are the
// other's first thread's.
function sameRanks(seen, one, other) {
  const map = new Map([[one.first, other.first]]);
  for (const [name, here, there] of seen) {
    if (!map.has(here)) map.set(here, there);
    assert.strictEqual(map.get(here), there, `${name} moved to another rank`);
  }
  assert.strictEqual(
    new Set(map.values()).size,
    map.size,
    'two threads map to one'
  );
}

// The lease record of `node` in the bucket, or null if there is none. An
// instance that left the cluster expired it (`expires_ms` 0).
async function record(bucket, nodeKeys, node) {
  const text = await bucket.get(`nodes/${nodeKeys.get(node.nodeId)}.json`);
  return text === null ? null : JSON.parse(text);
}

// A WebSocket to `url`:`next()` resolves to its next message, parsed.
async function openWebSocket(url) {
  const ws = new WebSocket(url);
  const received = [];
  const waiting = [];
  ws.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    const waiter = waiting.shift();
    if (waiter === undefined) received.push(message);
    else waiter(message);
  });
  await new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve, { once: true });
    ws.addEventListener(
      'error',
      () => reject(new Error(`no WebSocket to ${url}`)),
      {
        once: true,
      }
    );
  });
  return {
    send: (text) => ws.send(text),
    next: () =>
      received.length > 0
        ? Promise.resolve(received.shift())
        : new Promise((resolve) => waiting.push(resolve)),
    close: () => ws.close(1000),
  };
}

// ---------------------------------------------------------------------------
// Tests

// Instance A serves its objects on every thread it has; it drains, and
// instance B serves them from then on, with every write A acknowledged.
for (const [aThreads, bThreads] of [
  [THREADS, THREADS],
  [1, THREADS],
]) {
  test(
    `cluster threads: a draining instance on ${aThreads} hands objects of every thread to one on ${bThreads}`,
    UNIX_SIGNALS,
    async () => {
      await withCluster(
        2,
        async ({ nodes: [a, b], bucket, nodeKeys }) => {
          const A = await layout(a, aThreads);
          const B = await layout(b, bThreads);
          // On one thread, enough objects that B has some on each of its own.
          const byHome = await homes(A, 'handoff', aThreads > 1 ? 2 : 12);
          const names = [...byHome.values()].flat();
          for (const name of names) {
            for (let k = 0; k < 4; k++) {
              await get(a, `/write?name=${name}&key=k${k}&value=${name}-${k}`);
            }
          }
          // A request in flight in each object as A starts to drain, which
          // the handoff lets finish there.
          const held = names.map((name) =>
            fetchJson(
              a.httpPort,
              `/hold-increment?name=${name}&ms=3000&marker=held`,
              { timeoutMs: 20_000 }
            )
          );
          await until(
            async () => {
              const holding = await Promise.all(
                names.map((name) => get(a, `/holding?name=${name}`))
              );
              return holding.every((h) => h.holding === 1);
            },
            10_000,
            'the held requests never all ran'
          );
          // A write acknowledged just before the signal.
          await Promise.all(
            names.map((name) =>
              get(a, `/write?name=${name}&key=last&value=${name}-last`)
            )
          );
          const exit = a.stop({ signal: 'SIGTERM', timeoutMs: 20_000 });
          await sleep(300);
          // Requests that arrive meanwhile, through either instance, wait for
          // the handoff and run on B.
          const following = names.flatMap((name) =>
            [a, b].map((node) =>
              fetchJson(
                node.httpPort,
                `/increment?name=${name}&marker=after-${node.nodeId}`,
                { timeoutMs: 20_000 }
              ).then((answer) => ({ name, ...answer }))
            )
          );

          for (const [i, answer] of (await Promise.all(held)).entries()) {
            const { status, body } = answer;
            assert.strictEqual(status, 200, JSON.stringify(body));
            assert.strictEqual(body.nodeId, a.nodeId);
            assert.strictEqual(body.isolate, homeOf(byHome, names[i]));
          }
          const onB = new Map();
          for (const { name, status, body } of await Promise.all(following)) {
            assert.strictEqual(status, 200, JSON.stringify(body));
            assert.strictEqual(body.nodeId, b.nodeId, name);
            assert(B.all.includes(body.isolate), JSON.stringify(body));
            assert.strictEqual(onB.get(name) ?? body.isolate, body.isolate);
            onB.set(name, body.isolate);
          }

          assert.deepStrictEqual(await exit, { code: 0, signal: null });
          assert.doesNotMatch(a.stderr, /staying a cluster member/);
          assert.strictEqual(
            (await record(bucket, nodeKeys, a))?.expires_ms ?? 0,
            0,
            `${a.nodeId} is still a member`
          );

          // Each object, with every write A acknowledged and each increment
          // once, is B's alone.
          const seen = [];
          for (const name of names) {
            const state = await get(b, `/read?name=${name}`);
            assert.strictEqual(state.nodeId, b.nodeId);
            assert.strictEqual(state.isolate, onB.get(name));
            const rows = { last: `${name}-last` };
            for (let k = 0; k < 4; k++) rows[`k${k}`] = `${name}-${k}`;
            assert.deepStrictEqual(state.rows, rows, name);
            assert.deepStrictEqual(
              state.marks,
              { held: 1, [`after-${a.nodeId}`]: 1, [`after-${b.nodeId}`]: 1 },
              name
            );
            assert.strictEqual(
              await bucket.owner(state.id, UNIQUE_KEY),
              nodeKeys.get(b.nodeId)
            );
            seen.push([name, homeOf(byHome, name), state.isolate]);
          }
          if (aThreads === bThreads) sameRanks(seen, A, B);
          // B too runs them on threads other than its first, two of them.
          const others = new Set(
            seen.map(([, , there]) => there).filter((t) => t !== B.first)
          );
          assert.strictEqual(others.size, bThreads - 1, [...others].join());
        },
        cluster([aThreads, bThreads])
      );
    }
  );
}

// A peer's call arrives on the first thread of the object's owner, which
// passes it to the object's home.
test(
  "cluster threads: a peer's calls reach objects on threads other than the owner's first, and follow them through a drain",
  UNIX_SIGNALS,
  async () => {
    await withCluster(
      2,
      async ({ nodes: [a, b] }) => {
        const A = await layout(a, THREADS);
        const B = await layout(b, THREADS);
        const byHome = await homes(A, 'peer', 2);
        const names = A.others.flatMap((home) => byHome.get(home));

        for (const name of names) {
          const home = homeOf(byHome, name);
          const where = await get(b, `/where?name=${name}`);
          assert.deepStrictEqual(
            [where.nodeId, where.isolate],
            [a.nodeId, home]
          );
          const rpc = await get(b, `/rpc-where?name=${name}`);
          assert.deepStrictEqual([rpc.nodeId, rpc.isolate], [a.nodeId, home]);
          assert.strictEqual(rpc.instance, where.instance);

          // A body there and back.
          const sent = randomBytes(256 * 1024);
          const echoed = await fetch(
            `http://127.0.0.1:${b.httpPort}/echo?name=${name}`,
            { method: 'POST', body: sent }
          );
          assert.strictEqual(echoed.status, 200);
          assert.strictEqual(echoed.headers.get('x-isolate'), home);
          assert.strictEqual(echoed.headers.get('x-node'), a.nodeId);
          assert(Buffer.from(await echoed.arrayBuffer()).equals(sent));

          // A hibernatable WebSocket, accepted by the object on its home.
          const ws = await openWebSocket(
            `ws://127.0.0.1:${b.httpPort}/ws?name=${name}`
          );
          ws.send('ws');
          const message = await ws.next();
          assert.deepStrictEqual(
            [message.nodeId, message.isolate, message.count],
            [a.nodeId, home, 1]
          );
          ws.close();
        }

        // Through B, from before A drains until A has exited, increments by
        // fetch and by JS RPC, a call at a time in each lane. A JS RPC call
        // that reached A once the object was released fails as retryable,
        // and is sent again with its marker. None fails, none is lost and
        // none runs twice.
        let exited = false;
        const acked = new Map(names.map((name) => [name, []]));
        let retried = 0;
        const lane = async (name, kind) => {
          for (let i = 0, since = 0; since < 2; i++) {
            if (exited) since++;
            const marker = `${kind}-${i}`;
            const path =
              kind === 'rpc'
                ? `/rpc-increment?name=${name}&marker=${marker}`
                : `/increment?name=${name}&marker=${marker}`;
            const after = exited;
            let answer;
            for (let tries = 0; ; tries++) {
              answer = await fetchJson(b.httpPort, path, { timeoutMs: 20_000 });
              if (answer.status !== 500 || !answer.body.retryable) break;
              assert(tries < 5, JSON.stringify(answer.body));
              retried++;
            }
            assert.strictEqual(answer.status, 200, JSON.stringify(answer.body));
            acked.get(name).push({ marker, after, ...answer.body });
          }
        };
        const lanes = Promise.all(
          names.flatMap((name) => [lane(name, 'fetch'), lane(name, 'rpc')])
        );
        await sleep(500);
        const exit = a
          .stop({ signal: 'SIGTERM', timeoutMs: 20_000 })
          .finally(() => (exited = true));
        await lanes;
        assert.deepStrictEqual(await exit, { code: 0, signal: null });

        const seen = [];
        for (const name of names) {
          const answers = acked.get(name);
          const home = homeOf(byHome, name);
          let there;
          for (const answer of answers) {
            if (answer.nodeId === a.nodeId) {
              assert.strictEqual(answer.isolate, home, JSON.stringify(answer));
              assert(!answer.after, JSON.stringify(answer));
            } else {
              assert.strictEqual(answer.nodeId, b.nodeId);
              there ??= answer.isolate;
              assert.strictEqual(answer.isolate, there, JSON.stringify(answer));
            }
          }
          assert(there !== undefined, `${name} never moved to ${b.nodeId}`);
          assert(B.all.includes(there));
          seen.push([name, home, there]);
          const counts = answers.map((answer) => answer.count);
          assert.deepStrictEqual(
            [...counts].sort((x, y) => x - y),
            counts.map((_, i) => i + 2),
            name
          );
          const state = await get(b, `/read?name=${name}`);
          assert.strictEqual(state.nodeId, b.nodeId);
          const marks = { ws: 1 };
          for (const answer of answers) marks[answer.marker] = 1;
          assert.deepStrictEqual(state.marks, marks, name);
          assert.strictEqual(state.count, answers.length + 1);
        }
        sameRanks(seen, A, B);
        console.log(`JS RPC calls sent again: ${retried}`);
      },
      cluster([THREADS, THREADS])
    );
  }
);

// That a call is a peer's crosses from the first thread to the object's home.
// A peer's request that waits there for a handoff which ends only as the work
// in flight is cancelled is answered `moved`, and the peer runs it where the
// object went. Sent on from there, as the debug port's is, it would still run
// when the stopping instance's connections deadline cuts it off.
test(
  "cluster threads: a peer's request that a late handoff releases on a thread other than the first is answered moved",
  UNIX_SIGNALS,
  async () => {
    await withCluster(
      2,
      async ({ nodes: [a, b] }) => {
        const A = await layout(a, THREADS);
        const byHome = await homes(A, 'late', 1, [A.others[0]]);
        const [name] = byHome.get(A.others[0]);
        // Through the peer, and cancelled when the handoff's time for
        // requests in flight runs out, half of the stop timeout.
        const held = fetchJson(
          b.httpPort,
          `/hold-increment?name=${name}&ms=15000&marker=cut`,
          { timeoutMs: 30_000 }
        );
        await until(
          async () => (await get(a, `/holding?name=${name}`)).holding === 1,
          10_000,
          'the held request never ran'
        );
        const exit = a.stop({ signal: 'SIGTERM', timeoutMs: 30_000 });
        // Each thread says so once it marked its objects as being handed
        // off: from then on a request for one waits for its handoff.
        await until(
          () =>
            (a.stderr.match(/cluster: stopping: handing off/g) ?? []).length ===
            THREADS,
          10_000,
          'not every thread began its handoff'
        );
        const peer = await fetchJson(
          b.httpPort,
          `/hold-increment?name=${name}&ms=2000&marker=moved`,
          { timeoutMs: 30_000 }
        );
        assert.match(
          a.stderr,
          /a peer's request that waited for its handoff is answered moved/
        );
        assert.strictEqual(peer.status, 200, JSON.stringify(peer.body));
        assert.strictEqual(peer.body.nodeId, b.nodeId);
        const cut = await held;
        assert.strictEqual(cut.status, 500, JSON.stringify(cut.body));
        assert.deepStrictEqual(await exit, { code: 0, signal: null });
        const state = await get(b, `/read?name=${name}`);
        assert.deepStrictEqual(state.marks, { moved: 1 });
      },
      cluster([THREADS, 1], 'shortStop')
    );
  }
);

// An instance whose process is stopped (SIGSTOP) past its lease finds, once
// it runs again, that the lease lapsed: it serves none of its objects, open on
// a thread or not yet opened, whichever thread they live on, and exits. A
// peer then takes them over.
test(
  'cluster threads: an instance whose lease lapsed serves no object on any thread, and exits',
  UNIX_SIGNALS,
  async () => {
    await withCluster(
      2,
      async ({ nodes: [a, b], bucket, nodeKeys }) => {
        const A = await layout(a, THREADS);
        const B = await layout(b, THREADS);
        const byHome = await homes(A, 'lapse', 1);
        const opened = [...byHome.values()].flat();
        for (const name of opened) {
          await get(a, `/increment?name=${name}&marker=acked`);
        }
        // Objects A never opened; their homes are found on B below.
        const fresh = Array.from({ length: 24 }, (_, i) => `fresh-${i}`);

        a.signal('SIGSTOP');
        await sleep(LEASE_MS + 2000);
        // Queued in A's listen backlog while it is stopped. As A runs again,
        // its lease thread fences it, before or after its loop threads take
        // them: none may be served on A.
        const queued = [...opened, ...fresh].map((name) =>
          fetchJson(a.httpPort, `/increment?name=${name}&marker=stale`, {
            timeoutMs: 15_000,
          })
            .then((answer) => ({ name, ...answer }))
            .catch((error) => ({ name, error: String(error) }))
        );
        await sleep(200);
        const resumed = Date.now();
        a.signal('SIGCONT');
        const { code } = await a.exited({ timeoutMs: 5000 });
        const took = Date.now() - resumed;
        assert.strictEqual(code, 1);
        assert.match(a.stderr, /leaving the cluster: .*lapsed/);
        assert(took < 2000, `exited ${took} ms after it ran again`);

        // None was served on A; one A could not serve that B took over may
        // have been sent on to B.
        const servedOnB = new Set();
        for (const answer of await Promise.all(queued)) {
          if (answer.status !== 200) continue;
          assert.strictEqual(
            answer.body.nodeId,
            b.nodeId,
            JSON.stringify(answer)
          );
          servedOnB.add(answer.name);
        }

        const seen = [];
        for (const name of opened) {
          const state = await eventually(b, `/read?name=${name}`);
          assert.strictEqual(state.nodeId, b.nodeId);
          const marks = { acked: 1 };
          if (servedOnB.has(name)) marks.stale = 1;
          assert.deepStrictEqual(state.marks, marks, name);
          assert.strictEqual(
            await bucket.owner(state.id, UNIQUE_KEY),
            nodeKeys.get(b.nodeId)
          );
          seen.push([name, homeOf(byHome, name), state.isolate]);
        }
        sameRanks(seen, A, B);
        // The objects A had not opened were on every thread of A: B places
        // them on threads of the same ranks.
        const rankOf = new Map(seen.map(([, here, there]) => [there, here]));
        const ranks = new Set();
        for (const name of fresh) {
          const state = await eventually(b, `/read?name=${name}`);
          assert.deepStrictEqual(
            state.marks,
            servedOnB.has(name) ? { stale: 1 } : {},
            name
          );
          ranks.add(rankOf.get(state.isolate));
        }
        assert.deepStrictEqual(
          [...ranks].sort(),
          [...A.all].sort(),
          'the unopened objects were not on every thread of A'
        );
      },
      cluster([THREADS, THREADS])
    );
  }
);

// The lease thread watches every loop thread: one that makes no progress
// fences the instance although the others, the first among them, do.
for (const where of ['a thread other than the first', 'the first thread']) {
  test(`cluster threads: an instance fences when ${where} makes no progress`, async () => {
    await withCluster(
      2,
      async ({ nodes: [a, b], bucket, nodeKeys }) => {
        const A = await layout(a, THREADS);
        const byHome = await homes(A, 'stall', 1);
        const names = [...byHome.values()].flat();
        for (const name of names) {
          await get(a, `/increment?name=${name}&marker=acked`);
        }
        const firstThread = where === 'the first thread';
        const [spinner] = byHome.get(firstThread ? A.first : A.others[0]);
        // An object of another thread, which answers while the spinner spins.
        const [bystander] = byHome.get(firstThread ? A.others[0] : A.first);
        const armed = a.stderr.length;

        if (!firstThread) {
          // Control: a turn shorter than the stall time fences nothing.
          const spin = fetchJson(
            a.httpPort,
            `/spin?name=${spinner}&ms=${SHORT_LEASE_STALL_MS / 2}`,
            { timeoutMs: 20_000 }
          );
          await sleep(500);
          const asked = Date.now();
          await get(a, `/where?name=${bystander}`);
          assert(Date.now() - asked < 1000, 'the first thread was held up');
          const { status, body } = await spin;
          assert.strictEqual(status, 200, JSON.stringify(body));
          assert.strictEqual(body.isolate, homeOf(byHome, spinner));
          assert(body.end - body.start >= SHORT_LEASE_STALL_MS / 2);
          await sleep(SHORT_LEASE_STALL_MS / 2);
          assert.doesNotMatch(a.stderr.slice(armed), /leaving the cluster/);
          await get(a, `/increment?name=${spinner}&marker=after-spin`);
        }

        // A turn that never ends.
        const start = Date.now();
        const wedged = fetchJson(a.httpPort, `/wedge?name=${spinner}`).catch(
          () => {}
        );
        await sleep(500);
        // The other threads make progress meanwhile.
        for (let i = 0; i < 3; i++) {
          const asked = Date.now();
          await get(a, `/where?name=${bystander}`);
          assert(Date.now() - asked < 1000, 'another thread was held up');
          await sleep(500);
        }
        const { code } = await a.exited({
          timeoutMs: 4 * SHORT_LEASE_STALL_MS,
        });
        const took = Date.now() - start;
        assert.strictEqual(code, 1);
        const stalled =
          /leaving the cluster: its event loop made no progress for (\d+) ms/;
        const log = a.stderr.slice(armed);
        assert.match(log, stalled);
        const still = Number(log.match(stalled)[1]);
        assert(still >= SHORT_LEASE_STALL_MS, `stood still for ${still} ms`);
        assert(took >= SHORT_LEASE_STALL_MS, `fenced after ${took} ms`);
        await wedged;

        // B takes over every object, the spinner's too, with what A
        // acknowledged.
        for (const name of names) {
          const state = await eventually(b, `/read?name=${name}`);
          assert.strictEqual(state.nodeId, b.nodeId);
          const marks = { acked: 1 };
          if (name === spinner && !firstThread) marks['after-spin'] = 1;
          assert.deepStrictEqual(state.marks, marks, name);
          assert.strictEqual(
            await bucket.owner(state.id, UNIQUE_KEY),
            nodeKeys.get(b.nodeId)
          );
        }
      },
      cluster([THREADS, 1], 'shortLease')
    );
  });
}

// An alarm adopted from a dead peer runs on its object's home in the adopter.
test("cluster threads: a killed owner's alarms run once each, on their objects' homes in the adopter", async () => {
  await withCluster(
    2,
    async ({ nodes: [a, b], bucket, nodeKeys }) => {
      const A = await layout(a, THREADS);
      const B = await layout(b, THREADS);
      const byHome = await homes(A, 'alarm', 2);
      const names = [...byHome.values()].flat();
      const ids = new Map();
      // Due after A is dead, before its lease lapses.
      const due = Date.now() + 3000;
      for (const name of names) {
        const { id } = await get(a, `/set-alarm?name=${name}&at=${due}`);
        ids.set(name, id);
      }
      assert(Date.now() < due - 500, 'the alarms took too long to set');
      await a.stop({ signal: 'SIGKILL', timeoutMs: 2000 });

      // No request touches the objects: B finds the alarms in A's index once
      // A's lease lapsed, and claims each object as its alarm fires.
      const key = nodeKeys.get(b.nodeId);
      await until(
        async () => {
          const owners = await Promise.all(
            names.map((name) => bucket.owner(ids.get(name), UNIQUE_KEY))
          );
          return owners.every((owner) => owner === key);
        },
        30_000,
        `${b.nodeId} never claimed every object`
      );
      const fired = new Map();
      for (const name of names) {
        await until(
          async () => {
            const state = await get(b, `/alarms?name=${name}`);
            fired.set(name, state);
            return state.fired.length > 0;
          },
          10_000,
          `the alarm of ${name} never ran`
        );
      }
      // Long enough for a second run to show.
      await sleep(2000);
      const seen = [];
      for (const name of names) {
        const state = await get(b, `/alarms?name=${name}`);
        assert.strictEqual(state.nodeId, b.nodeId);
        assert.strictEqual(state.isolate, fired.get(name).isolate);
        assert.deepStrictEqual(
          state.fired,
          [{ nodeId: b.nodeId, isolate: state.isolate }],
          name
        );
        seen.push([name, homeOf(byHome, name), state.isolate]);
      }
      sameRanks(seen, A, B);
      const ran = seen.map(([, , there]) => there);
      assert(ran.includes(B.first), 'no alarm ran on the first thread');
      assert.strictEqual(
        new Set(ran.filter((t) => t !== B.first)).size,
        THREADS - 1,
        'the alarms ran on too few other threads'
      );
    },
    cluster([THREADS, THREADS])
  );
});

// A thread that has not stopped its objects by the release deadline, here
// because it runs JavaScript throughout, keeps its instance a member of the
// cluster, so that peers take that thread's objects over only once its lease
// lapsed; the other threads' objects move at once. The instance exits at its
// stop deadline with that thread still running.
test(
  'cluster threads: an instance one of whose threads cannot stop in time exits by its stop deadline as a member, and its objects move as its lease lapses',
  UNIX_SIGNALS,
  async () => {
    await withCluster(
      2,
      async ({ nodes: [a, b], bucket, nodeKeys }) => {
        const A = await layout(a, THREADS);
        const byHome = await homes(A, 'stuck', 1);
        const [stuckThread, cleanThread] = A.others;
        const [stuck] = byHome.get(stuckThread);
        const clean = [...byHome.get(A.first), ...byHome.get(cleanThread)];
        for (const name of [stuck, ...clean]) {
          await get(a, `/increment?name=${name}&marker=acked`);
        }
        // The stuck object's thread runs JavaScript from before the signal
        // until well past the stop deadline (and within the stall time).
        const signalAt = Date.now() + 500;
        const spinUntil = signalAt + SHORT_STOP_MS + 3000;
        const spin = fetchJson(
          a.httpPort,
          `/spin?name=${stuck}&until=${spinUntil}`,
          { timeoutMs: 20_000 }
        ).catch((error) => ({ error: String(error) }));
        await sleep(signalAt - Date.now());
        const exiting = a.stop({ signal: 'SIGTERM', timeoutMs: 20_000 });

        // What the other threads hand off B serves while A still drains.
        await sleep(1500);
        for (const name of clean) {
          const state = await get(b, `/read?name=${name}`);
          assert.strictEqual(state.nodeId, b.nodeId);
          assert.deepStrictEqual(state.marks, { acked: 1 }, name);
        }

        const exit = await exiting;
        const stoppedIn = Date.now() - signalAt;
        assert.deepStrictEqual(exit, { code: 0, signal: null });
        await spin;
        const expiresMs = (await record(bucket, nodeKeys, a))?.expires_ms ?? 0;
        // What A left behind, once its lease lapsed, with what A
        // acknowledged.
        const state = await eventually(b, `/read?name=${stuck}`);
        const servedAt = Date.now();
        assert.strictEqual(state.nodeId, b.nodeId);
        assert.deepStrictEqual(state.marks, { acked: 1 });

        // How A ended, all at once. It waited for the thread until its stop
        // deadline, and no longer: not for the spin to end.
        const wrong = [];
        if (stoppedIn < SHORT_STOP_MS || stoppedIn >= SHORT_STOP_MS + 1000) {
          wrong.push(
            `stopped ${stoppedIn} ms after the signal, its thread's spin ` +
              `${spinUntil - signalAt} ms after it`
          );
        }
        for (const line of [
          /loop thread \d+ has not stopped its Durable Objects at the release deadline/,
          /staying a cluster member/,
          /a loop thread still runs at the stop deadline/,
        ]) {
          if (line.test(a.stderr)) continue;
          const said = a.stderr.match(/.*(leaving the cluster|deadline).*/g);
          wrong.push(`no ${line}, but: ${said}`);
        }
        if (expiresMs === 0) wrong.push(`${a.nodeId} left the cluster`);
        if (servedAt < expiresMs) {
          wrong.push(`B served ${stuck} before A's lease lapsed`);
        }
        assert.deepStrictEqual(wrong, []);
      },
      cluster([THREADS, 1], 'shortStop')
    );
  }
);

// Work an object being handed off does is admitted on other threads, so a
// call cycle through two threads completes; a request it did not cause waits
// for the handoff.
test(
  "cluster threads: a call cycle through two threads' objects completes through a handoff, and other requests follow it",
  UNIX_SIGNALS,
  async () => {
    await withCluster(
      2,
      async ({ nodes: [a, b] }) => {
        const A = await layout(a, THREADS);
        const byHome = await homes(A, 'admit', 1);
        const [x] = byHome.get(A.others[0]);
        const [y] = byHome.get(A.others[1]);
        for (const name of [x, y]) {
          await get(a, `/increment?name=${name}&marker=first`);
        }
        // Y has a request in flight for most of the time its handoff allows
        // one, so its handoff is under way, and Y still on A, when X calls.
        const HOLD_MS = IN_FLIGHT_MS - 500;
        const held = fetchJson(
          a.httpPort,
          `/hold-increment?name=${y}&ms=${HOLD_MS}&marker=held`,
          { timeoutMs: 20_000 }
        );
        // X waits, then calls Y, which calls X back, as A drains.
        const startedAt = Date.now();
        const cycle = fetchJson(
          a.httpPort,
          `/chain?name=${x}&ms=1500&targets=${y},${x}`,
          { timeoutMs: 20_000 }
        );
        await sleep(500);
        const exit = a.stop({ signal: 'SIGTERM', timeoutMs: 20_000 });
        await sleep(300);
        const unrelated = [a, b].map((node) =>
          fetchJson(
            node.httpPort,
            `/increment?name=${x}&marker=unrelated-${node.nodeId}`,
            { timeoutMs: 20_000 }
          )
        );
        const { status, body } = await cycle;
        assert.strictEqual(status, 200, JSON.stringify(body));
        assert.deepStrictEqual(
          [body.nodeId, body.isolate, body.count],
          [a.nodeId, homeOf(byHome, x), 3]
        );
        // Y's thread admitted X's call, as the work of an object another
        // thread hands off: it did not wait for Y's handoff, which ends once
        // Y's held request has.
        const took = Date.now() - startedAt;
        assert(
          took < HOLD_MS - 1000,
          `the cycle took ${took} ms: it waited for a handoff`
        );
        const kept = await held;
        assert.strictEqual(kept.status, 200, JSON.stringify(kept.body));
        assert.deepStrictEqual(
          [kept.body.nodeId, kept.body.isolate],
          [a.nodeId, homeOf(byHome, y)]
        );
        for (const answer of await Promise.all(unrelated)) {
          assert.strictEqual(answer.status, 200, JSON.stringify(answer.body));
          assert.strictEqual(answer.body.nodeId, b.nodeId);
        }
        assert.deepStrictEqual(await exit, { code: 0, signal: null });
        const states = await Promise.all(
          [x, y].map((name) => get(b, `/read?name=${name}`))
        );
        assert.deepStrictEqual(
          states.map((s) => [s.nodeId, s.count]),
          [
            [b.nodeId, 5],
            [b.nodeId, 3],
          ]
        );
        assert.strictEqual(states[0].marks[`unrelated-${a.nodeId}`], 1);
        assert.strictEqual(states[0].marks[`unrelated-${b.nodeId}`], 1);
      },
      cluster([THREADS, 1])
    );
  }
);

// Which objects an instance hands off is known on all of its threads. A call
// that an object of one thread made before the drain, through an object of a
// peer, reaches an object of another thread once both are being handed off:
// that thread admits it as the first object's work, although nothing in the
// call says that a handoff made it.
test(
  "cluster threads: a thread admits the work of an object another thread hands off, which reaches it through a peer's object",
  UNIX_SIGNALS,
  async () => {
    await withCluster(
      2,
      async ({ nodes: [a, b] }) => {
        const A = await layout(a, THREADS);
        const byHome = await homes(A, 'across', 1);
        const [x] = byHome.get(A.others[0]);
        const [y] = byHome.get(A.others[1]);
        const z = 'across-on-b';
        assert.strictEqual((await get(b, `/where?name=${z}`)).nodeId, b.nodeId);
        // Y is on A, its handoff under way, when the call reaches it.
        const HOLD_MS = IN_FLIGHT_MS - 500;
        const held = fetchJson(
          a.httpPort,
          `/hold-increment?name=${y}&ms=${HOLD_MS}&marker=held`,
          { timeoutMs: 20_000 }
        );
        // X calls Z at once, before A drains; Z waits, then calls Y.
        const startedAt = Date.now();
        const chain = fetchJson(
          a.httpPort,
          `/chain?name=${x}&next=1500&targets=${z},${y}`,
          { timeoutMs: 20_000 }
        );
        await sleep(500);
        const exit = a.stop({ signal: 'SIGTERM', timeoutMs: 20_000 });
        const { status, body } = await chain;
        const took = Date.now() - startedAt;
        assert.strictEqual(status, 200, JSON.stringify(body));
        assert.deepStrictEqual(
          [body.nodeId, body.isolate],
          [a.nodeId, homeOf(byHome, x)]
        );
        assert(took >= 1500, `Z called Y ${took} ms in, before A drained`);
        // Not once Y's handoff ended, which waits for its held request.
        assert(
          took < HOLD_MS - 1000,
          `the chain took ${took} ms: it waited for a handoff`
        );
        const kept = await held;
        assert.strictEqual(kept.status, 200, JSON.stringify(kept.body));
        assert.deepStrictEqual(
          [kept.body.nodeId, kept.body.isolate, kept.body.count],
          [a.nodeId, homeOf(byHome, y), 2]
        );
        assert.deepStrictEqual(await exit, { code: 0, signal: null });
      },
      cluster([THREADS, 1])
    );
  }
);

// Each thread serves a `bucket` service of the cluster's bucket itself.
test("cluster threads: every thread of every instance reads what any wrote to the cluster's bucket", async () => {
  await withCluster(
    2,
    async ({ nodes: [a, b], bucket }) => {
      const A = await layout(a, THREADS);
      const B = await layout(b, THREADS);
      const threads = [...A.threads, ...B.threads];
      const values = new Map();
      for (const [i, thread] of threads.entries()) {
        // Every other one over 1 MiB.
        const value = randomBytes(i % 2 === 0 ? 1536 * 1024 : 1024);
        const put = await thread.request(`/bucket/key-${i}`, {
          method: 'PUT',
          body: value,
        });
        assert.strictEqual(put.status, 200, put.body.toString());
        values.set(`key-${i}`, value);
      }
      const stored = await bucket.list('services/blobs/');
      assert.deepStrictEqual(
        stored.sort(),
        [...values.keys()].map((key) => `services/blobs/${key}`).sort()
      );
      for (const thread of threads) {
        for (const [key, value] of values) {
          const got = await thread.request(`/bucket/${key}`);
          assert.strictEqual(got.status, 200, key);
          assert.strictEqual(got.headers['x-isolate'], thread.thread);
          assert(got.body.equals(value), `${key} through ${thread.thread}`);
        }
      }

      // While A's first thread runs JavaScript, another of its threads reads
      // the bucket at once. The `cache` service is the first thread's: a
      // match in it waits, which shows that the spin holds what the first
      // thread serves.
      const other = A.threads.find((t) => t.thread === A.others[0]);
      // An instance stores entries once it has been a member for a while.
      await until(
        async () => {
          const put = await other.request('/cache/entry', {
            method: 'PUT',
            body: Buffer.from('cached'),
          });
          assert.strictEqual(put.status, 204, put.body.toString());
          return (await other.request('/cache/entry')).status === 200;
        },
        20_000,
        'the cache stored nothing'
      );
      const byHome = await homes(A, 'blocker', 1, [A.first]);
      const [blocker] = byHome.get(A.first);
      const spin = get(a, `/spin?name=${blocker}&ms=3000`);
      await sleep(500);
      const read = await other.request('/bucket/key-0');
      const readAt = Date.now();
      const matched = await other.request('/cache/entry');
      const matchedAt = Date.now();
      const { isolate, end } = await spin;
      assert.strictEqual(isolate, A.first);
      assert.strictEqual(read.status, 200);
      assert(read.body.equals(values.get('key-0')));
      assert(readAt < end, `the read waited ${readAt - end} ms for the spin`);
      // A cache may miss, as the cluster's does for a key whose home instance
      // answers late; whatever it answers, it answers after the spin.
      if (matched.status === 200) {
        assert.strictEqual(matched.body.toString(), 'cached');
      } else {
        assert.strictEqual(matched.status, 404);
      }
      assert(matchedAt >= end, 'the match did not wait for the first thread');
    },
    cluster([THREADS, THREADS])
  );
});
