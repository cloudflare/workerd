// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Integration test for cluster mode. The test starts a fake S3 (fake-s3.js, in
// a workerd of its own) and several workerd instances whose cluster state lives
// in it, each with its SQLite files on a disk of its own, then exercises:
//   1. Consistent DO state across instances (single-writer correctness).
//   2. Transparent forwarding when a request arrives at the "wrong" instance.
//   3. Takeover after a node is killed.
//   4. Each running instance holds a lease record in the bucket.
//   5. Storing/loading a DO stub without calling it claims nothing.
//   6. A broken DO does not stay stuck while stubs still pin it: it can be
//      re-instantiated instead of hanging requests.
//   7. Persistent stubs to restored RpcTargets rooted at a DO on one node can be
//      stored and redeemed from a DO on another node; the restore runs on the
//      root DO's owner. This holds whether the stub arrives as a token or was
//      minted in-process and passed through props.
//   8. A stub that has resolved to a remote DO keeps failing once that DO
//      breaks, while a fresh stub reaches the re-instantiated DO.

import { spawn } from 'node:child_process';
import { mkdir, mkdtemp, readdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { connect, createServer } from 'node:net';
import { createHash, createHmac, randomBytes } from 'node:crypto';
import { join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';
import { setTimeout as sleep } from 'node:timers/promises';

assert(
  env.WORKERD_BINARY !== undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert(
  env.WD_TEST_CONFIG !== undefined,
  'You must set the WD_TEST_CONFIG environment variable.'
);
assert(
  env.FAKE_S3_CONFIG !== undefined,
  'You must set the FAKE_S3_CONFIG environment variable.'
);

const CONTROL_FD = 3;

// Node's SIGTERM is TerminateProcess on Windows, so a node cannot drain there.
const SIGTERM_DRAINS = {
  skip: process.platform === 'win32' && 'SIGTERM does not drain on Windows',
};

// A minimal harness specialized for the cluster test. Unlike server-harness.mjs
// it:
//   - allows overriding bindings via process environment (NODE_ID),
//   - points each instance at the fake S3 and at a disk of its own,
//   - supports SIGKILL in addition to SIGTERM for the takeover test.
class ClusterNode {
  #binary;
  #config;
  #localPath;
  #s3Port;
  #nodeId;
  #child = null;
  #httpPort = null;
  #closed = null;
  #stderrBuffer = '';
  #extraArgs;

  constructor({ binary, config, localPath, s3Port, nodeId, extraArgs = [] }) {
    this.#extraArgs = extraArgs;
    this.#binary = binary;
    this.#config = config;
    this.#localPath = localPath;
    this.#s3Port = s3Port;
    this.#nodeId = nodeId;
  }

  get localPath() {
    return this.#localPath;
  }

  get nodeId() {
    return this.#nodeId;
  }

  get httpPort() {
    assert(this.#httpPort !== null, 'node not started');
    return this.#httpPort;
  }

  // Returns the contents of the node's stderr observed so far.
  get stderr() {
    return this.#stderrBuffer;
  }

  async start() {
    assert.strictEqual(this.#child, null);

    const args = [
      'serve',
      this.#config,
      '--experimental',
      '--verbose',
      `--control-fd=${CONTROL_FD}`,
      `--socket-addr=http=127.0.0.1:0`,
    ];
    if (this.#localPath !== undefined) {
      args.push(`--directory-path=local=${this.#localPath}`);
    }
    args.push(...this.#extraArgs);

    const child = spawn(this.#binary, args, {
      stdio: ['ignore', 'inherit', 'pipe', 'pipe'],
      env: {
        ...env,
        // Drop the runtime on exit, joining its threads, which TSan reports
        // as leaked otherwise.
        KJ_CLEAN_SHUTDOWN: '1',
        NODE_ID: this.#nodeId,
        AWS_ENDPOINT_URL: `http://127.0.0.1:${this.#s3Port}`,
      },
    });
    this.#child = child;

    child.stderr.on('data', (data) => {
      const chunk = data.toString('utf8');
      this.#stderrBuffer += chunk;
      // Mirror to our own stderr so test output remains useful.
      process.stderr.write(`[${this.#nodeId}] ${chunk}`);
    });

    // Watch the control fd for the assigned http port.
    const portPromise = new Promise((resolve, reject) => {
      let buffer = '';
      const onData = (data) => {
        buffer += data.toString('utf8');
        let nl;
        while ((nl = buffer.indexOf('\n')) !== -1) {
          const line = buffer.slice(0, nl).trim();
          buffer = buffer.slice(nl + 1);
          if (!line) continue;
          try {
            const parsed = JSON.parse(line);
            if (parsed.event === 'listen' && parsed.socket === 'http') {
              child.stdio[CONTROL_FD].off('data', onData);
              resolve(parsed.port);
              return;
            }
          } catch (err) {
            reject(
              new Error(
                `Failed to parse control message from node ${this.#nodeId}: ${line}`
              )
            );
            return;
          }
        }
      };
      child.stdio[CONTROL_FD].on('data', onData);
      child.once('error', reject);
      child.once('exit', (code, signal) => {
        if (this.#httpPort === null) {
          reject(
            new Error(
              `Node ${this.#nodeId} exited before listening (code=${code}, signal=${signal})`
            )
          );
        }
      });
    });

    this.#closed = new Promise((resolve) => {
      child.once('exit', (code, signal) => resolve({ code, signal }));
    });

    await new Promise((resolve, reject) => {
      child.once('spawn', resolve).once('error', reject);
    });

    this.#httpPort = await portPromise;
  }

  async stop({ signal = 'SIGTERM', timeoutMs = 10_000 } = {}) {
    if (this.#child === null) return null;
    const child = this.#child;
    this.#child = null;
    child.kill(signal);

    const killTimer = setTimeout(() => {
      try {
        child.kill('SIGKILL');
      } catch (_) {
        // process may already be gone.
      }
    }, timeoutMs);

    const result = await this.#closed;
    clearTimeout(killTimer);
    this.#httpPort = null;
    return result;
  }
}

// Fetches `path` from the given node and parses the JSON response. When
// `timeoutMs` is given, a request that does not complete in time fails with a
// clear error instead of hanging the test (used where a bug would manifest as
// a request that never completes).
async function fetchJson(port, path, { timeoutMs } = {}) {
  const url = `http://127.0.0.1:${port}${path}`;
  let res;
  try {
    res = await fetch(url, {
      signal:
        timeoutMs === undefined ? undefined : AbortSignal.timeout(timeoutMs),
    });
  } catch (err) {
    if (err.name === 'TimeoutError') {
      throw new Error(
        `request to ${url} did not complete within ${timeoutMs}ms`
      );
    }
    throw err;
  }
  const text = await res.text();
  let body;
  try {
    body = JSON.parse(text);
  } catch (err) {
    throw new Error(
      `Non-JSON response from ${url} (status ${res.status}): ${text}`
    );
  }
  return { status: res.status, body };
}

// The fake S3 the cluster's state lives in, in a workerd of its own.
class FakeS3 {
  #node;

  constructor() {
    // A cluster node's harness starts any workerd; this one has no cluster.
    this.#node = new ClusterNode({
      binary: env.WORKERD_BINARY,
      config: env.FAKE_S3_CONFIG,
      nodeId: 's3',
    });
  }

  get port() {
    return this.#node.httpPort;
  }

  start() {
    return this.#node.start();
  }

  stop() {
    return this.#node.stop({ timeoutMs: 5000 });
  }

  // The keys under `prefix` in the cluster's bucket, whose layout is under
  // `cluster/v1/`.
  async list(prefix) {
    const query = new URLSearchParams({
      'list-type': '2',
      prefix: `cluster/v1/${prefix}`,
    });
    const res = await fetch(`http://127.0.0.1:${this.port}/wdtest/?${query}`);
    const xml = await res.text();
    return [...xml.matchAll(/<Key>([^<]*)<\/Key>/g)].map((m) =>
      m[1].slice('cluster/v1/'.length)
    );
  }

  async get(key) {
    const res = await fetch(
      `http://127.0.0.1:${this.port}/wdtest/cluster/v1/${key}`
    );
    return res.status === 404 ? null : await res.text();
  }

  // The key of the node that owns the Counter with this ID, or null when no
  // owner record exists.
  async owner(id) {
    const record = await this.get(`${objectPrefix(id)}/own.json`);
    return record === null ? null : JSON.parse(record).node;
  }

  // How many GET, PUT and DELETE requests the bucket served for `key`.
  async requests(key) {
    const query = new URLSearchParams({ key });
    return (
      await fetch(`http://127.0.0.1:${this.port}/_requests?${query}`)
    ).json();
  }

  // The keys of the nodes with a lease record.
  async nodes() {
    // Not the `.probe` a starting node writes and deletes.
    return (await this.list('nodes/'))
      .filter((key) => key.endsWith('.json'))
      .map((key) => key.slice('nodes/'.length, -'.json'.length));
  }
}

// Starts a fake S3 and `numNodes` workerd instances and runs `fn` with:
//   nodes      the ClusterNode instances, in start order
//   bucket     the FakeS3
//   nodeKeys   Map from nodeId to the node's key (32-char hex), which is also
//              what owner records name
async function withCluster(numNodes, fn, { extraArgs = () => [] } = {}) {
  // Each test run gets its own bucket and disks, so state doesn't leak
  // between tests.
  const root = await mkdtemp(join(tmpdir(), 'workerd-cluster-'));
  const bucket = new FakeS3();
  const nodes = [];
  const nodeKeys = new Map();
  try {
    await bucket.start();
    for (let i = 0; i < numNodes; i++) {
      const localPath = join(root, `node${i}`);
      await mkdir(localPath);
      const node = new ClusterNode({
        binary: env.WORKERD_BINARY,
        config: env.WD_TEST_CONFIG,
        localPath,
        s3Port: bucket.port,
        nodeId: `node${i}`,
        extraArgs: await extraArgs(i),
      });
      await node.start();
      nodes.push(node);

      // A node writes its lease record in its first maintenance round. Since
      // nodes start one at a time, the record that is new is this node's.
      const known = new Set(nodeKeys.values());
      const deadline = Date.now() + 10_000;
      let fresh = [];
      while (Date.now() < deadline) {
        fresh = (await bucket.nodes()).filter((key) => !known.has(key));
        if (fresh.length > 0) break;
        await sleep(50);
      }
      assert.strictEqual(
        fresh.length,
        1,
        `expected exactly one new lease record after starting ${node.nodeId}, got: [${fresh.join(', ')}]`
      );
      nodeKeys.set(node.nodeId, fresh[0]);
    }
    await fn({ nodes, bucket, nodeKeys });
  } finally {
    // Stop all nodes (ignore errors -- some may already be stopped by the
    // test itself).
    for (const node of nodes) {
      try {
        await node.stop({ timeoutMs: 5000 });
      } catch (_) {
        // ignore
      }
    }
    await bucket.stop();
    await rm(root, { recursive: true, force: true });
  }
}

// ---------------------------------------------------------------------------
// Tests

test('cluster: DO state is consistent across instances (single writer)', async () => {
  await withCluster(3, async ({ nodes }) => {
    // Send a series of /increment requests to a round-robin of nodes, all for
    // the same DO name. Track the maximum returned count -- it must be a
    // strictly-increasing sequence (1, 2, 3, ...) because only one instance is
    // the writer at any given time and the DO state is persistent.
    const totalRequests = 12;
    const expected = [];
    for (let i = 1; i <= totalRequests; i++) expected.push(i);

    const observed = [];
    const ownerIds = new Set();
    for (let i = 0; i < totalRequests; i++) {
      const node = nodes[i % nodes.length];
      const { status, body } = await fetchJson(
        node.httpPort,
        '/increment?name=consistency'
      );
      assert.strictEqual(
        status,
        200,
        `request to ${node.nodeId} failed: ${JSON.stringify(body)}`
      );
      observed.push(body.count);
      ownerIds.add(body.nodeId);
    }
    observed.sort((a, b) => a - b);
    assert.deepStrictEqual(
      observed,
      expected,
      `each /increment must be a unique strictly-increasing count`
    );

    // The DO is owned by exactly one node at a time. Even though we directed
    // requests at all 3 nodes, every observed response should report the same
    // nodeId -- the actual owner -- because incoming requests are forwarded.
    assert.strictEqual(
      ownerIds.size,
      1,
      `expected exactly one DO owner across ${totalRequests} requests, got: ${[...ownerIds].join(', ')}`
    );
  });
});

test('cluster: requests to non-owner are transparently forwarded', async () => {
  await withCluster(2, async ({ nodes }) => {
    // Prime the DO by sending an increment to node 0. That node should claim
    // ownership and respond. Then send a /get to node 1. Node 1 should forward
    // to node 0 (the owner) and return the same state, and node 0 should be
    // identified as the responder.
    const first = await fetchJson(
      nodes[0].httpPort,
      '/increment?name=forwarding'
    );
    assert.strictEqual(first.status, 200);
    assert.strictEqual(first.body.count, 1);
    const owner = first.body.nodeId;

    const second = await fetchJson(nodes[1].httpPort, '/get?name=forwarding');
    assert.strictEqual(second.status, 200);
    assert.strictEqual(
      second.body.count,
      1,
      `forwarded /get must see prior increment`
    );
    assert.strictEqual(
      second.body.nodeId,
      owner,
      `forwarded request must be served by the original owner (${owner}), not the forwarding node (${nodes[1].nodeId})`
    );

    // Same id must be reported for both calls (sanity check for idFromName
    // determinism across instances).
    assert.strictEqual(first.body.id, second.body.id);
  });
});

test('cluster: killing the owner allows another node to take over', async () => {
  await withCluster(2, async ({ nodes }) => {
    // Prime the DO on whichever node ends up owning it.
    const initial = await fetchJson(
      nodes[0].httpPort,
      '/increment?name=takeover'
    );
    assert.strictEqual(initial.status, 200);
    assert.strictEqual(initial.body.count, 1);
    const originalOwner = initial.body.nodeId;
    const ownerIndex = nodes.findIndex((n) => n.nodeId === originalOwner);
    assert.notStrictEqual(ownerIndex, -1);
    const survivorIndex = ownerIndex === 0 ? 1 : 0;
    const survivor = nodes[survivorIndex];

    // Hard-kill the owner. SIGKILL leaves its lease record in place, so the
    // survivor takes over once that lease expires.
    await nodes[ownerIndex].stop({ signal: 'SIGKILL', timeoutMs: 2000 });

    // Hit the survivor repeatedly. Requests fail until the dead owner's lease
    // expires; then the survivor takes ownership. The persisted count must be
    // preserved. Each request is bounded: one that hung once held this test
    // for nine minutes.
    let response = null;
    let lastErr = null;
    const deadline = Date.now() + 30_000;
    while (Date.now() < deadline) {
      try {
        response = await fetchJson(
          survivor.httpPort,
          '/increment?name=takeover',
          { timeoutMs: 10_000 }
        );
        if (response.status === 200) break;
      } catch (err) {
        lastErr = err;
      }
      await sleep(200);
    }
    assert(
      response !== null && response.status === 200,
      `survivor never succeeded; last error: ${lastErr}`
    );
    assert.strictEqual(
      response.body.nodeId,
      survivor.nodeId,
      `survivor (${survivor.nodeId}) must own the DO after takeover, got ${response.body.nodeId}`
    );
    assert.strictEqual(
      response.body.count,
      2,
      `persisted state must be preserved across takeover`
    );

    // Further requests to the survivor continue to work and increment.
    const followup = await fetchJson(
      survivor.httpPort,
      '/increment?name=takeover'
    );
    assert.strictEqual(followup.status, 200);
    assert.strictEqual(followup.body.count, 3);
    assert.strictEqual(followup.body.nodeId, survivor.nodeId);
  });
});

test(
  'cluster: draining node still routes in-flight requests to a remote owner',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(2, async ({ nodes }) => {
      // Prime the DO so that exactly one node owns it.
      const initial = await fetchJson(
        nodes[0].httpPort,
        '/increment?name=drain'
      );
      assert.strictEqual(initial.status, 200);
      assert.strictEqual(initial.body.count, 1);
      const owner = nodes.find((n) => n.nodeId === initial.body.nodeId);
      const nonOwner = nodes.find((n) => n.nodeId !== initial.body.nodeId);
      assert(owner !== undefined && nonOwner !== undefined);

      // Put a request in flight on the non-owner whose stateless handler waits
      // before calling the DO, then SIGTERM the non-owner while it is waiting.
      // Draining lets in-flight requests finish, so this request must still be
      // able to route to the owner over cluster RPC after draining began.
      const requestPromise = fetchJson(
        nonOwner.httpPort,
        '/increment?name=drain&delay=1000',
        { timeoutMs: 10_000 }
      );
      await sleep(250);
      const exitPromise = nonOwner.stop({
        signal: 'SIGTERM',
        timeoutMs: 10_000,
      });

      const response = await requestPromise;
      assert.strictEqual(
        response.status,
        200,
        `in-flight request on draining node failed: ${JSON.stringify(response.body)}`
      );
      assert.strictEqual(response.body.nodeId, owner.nodeId);
      assert.strictEqual(response.body.count, 2);

      // The draining node must have exited cleanly once the request finished.
      const exit = await exitPromise;
      assert.deepStrictEqual(exit, { code: 0, signal: null });
    });
  }
);

test(
  'cluster: draining node refuses to claim an unowned DO',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(2, async ({ nodes, bucket, nodeKeys }) => {
      // Put a request in flight on node 0 for a DO that nobody owns yet, then
      // SIGTERM node 0 while its stateless handler is still waiting. By the time
      // the handler calls the DO, node 0 is draining: its cluster listener no
      // longer accepts connections, so claiming the DO there would leave it
      // unreachable from the rest of the cluster until node 0 exits. The claim
      // must be refused instead.
      const requestPromise = fetchJson(
        nodes[0].httpPort,
        '/increment?name=lame-duck&delay=1000',
        { timeoutMs: 10_000 }
      );
      await sleep(250);
      const exitPromise = nodes[0].stop({
        signal: 'SIGTERM',
        timeoutMs: 10_000,
      });

      const response = await requestPromise;
      assert.strictEqual(
        response.status,
        500,
        `draining node must not claim the DO: ${JSON.stringify(response.body)}`
      );
      // The refusal is a DISCONNECTED exception, which JS sees as a generic
      // connection-loss error.
      assert.match(response.body.error, /Network connection lost/);

      const exit = await exitPromise;
      assert.deepStrictEqual(exit, { code: 0, signal: null });

      // The DO was never instantiated anywhere: the survivor claims it fresh.
      const survivor = await fetchJson(
        nodes[1].httpPort,
        '/increment?name=lame-duck'
      );
      assert.strictEqual(survivor.status, 200, JSON.stringify(survivor.body));
      assert.strictEqual(survivor.body.count, 1);
      assert.strictEqual(survivor.body.nodeId, nodes[1].nodeId);
      assert.strictEqual(
        await bucket.owner(survivor.body.id),
        nodeKeys.get(nodes[1].nodeId)
      );
    });
  }
);

test(
  "cluster: a peer takes over a draining owner's DO before its lease ends",
  SIGTERM_DRAINS,
  async () => {
    await withCluster(2, async ({ nodes }) => {
      const initial = await fetchJson(
        nodes[0].httpPort,
        '/increment?name=handoff'
      );
      assert.strictEqual(initial.status, 200);
      const owner = nodes.find((n) => n.nodeId === initial.body.nodeId);
      const peer = nodes.find((n) => n.nodeId !== initial.body.nodeId);

      // A slow request holds the owner draining for longer than a peer
      // needs to see it leave, but within its lease.
      const holding = fetchJson(
        owner.httpPort,
        '/increment?name=hold&delay=12000',
        { timeoutMs: 20_000 }
      );
      await sleep(250);
      const drainedAt = Date.now();
      const exitPromise = owner.stop({ signal: 'SIGTERM', timeoutMs: 20_000 });

      // A request waits on the owner until the peer's next listing drops
      // it, then the peer claims. One timed out would still increment.
      let response = null;
      while (Date.now() < drainedAt + 10_000) {
        response = await fetchJson(peer.httpPort, '/increment?name=handoff', {
          timeoutMs: drainedAt + 10_000 - Date.now(),
        }).catch(() => null);
        if (response?.status === 200) break;
        await sleep(200);
      }
      assert.strictEqual(response?.status, 200, JSON.stringify(response?.body));
      assert.strictEqual(response.body.nodeId, peer.nodeId);
      assert.strictEqual(response.body.count, 2);
      await holding.catch(() => {});
      assert.deepStrictEqual(await exitPromise, { code: 0, signal: null });
    });
  }
);

test('cluster: concurrent first requests to a DO read its owner record once per node', async () => {
  await withCluster(2, async ({ nodes, bucket }) => {
    // Without coalescing, each of these reads own.json and the losers retry
    // their claims: the stampede that starved lease renewal on the fleet.
    const results = await Promise.all(
      Array.from({ length: 64 }, (_, i) =>
        fetchJson(nodes[i % 2].httpPort, '/increment?name=herd')
      )
    );
    for (const r of results)
      assert.strictEqual(r.status, 200, JSON.stringify(r.body));
    const counts = results.map((r) => r.body.count).sort((a, b) => a - b);
    assert.deepStrictEqual(
      counts,
      Array.from({ length: 64 }, (_, i) => i + 1)
    );
    const requests = await bucket.requests(
      `${objectPrefix(results[0].body.id)}/own.json`
    );
    assert(requests <= 8, `${requests} owner record requests for one cold DO`);
  });
});

// Where a Durable Object's records are in the bucket: a hash of its
// namespace's uniqueKey, then its ID.
function objectPrefix(id, uniqueKey = 'counter-test-namespace') {
  const hash = createHash('sha256');
  for (const part of ['workerd cluster namespace v1', '', uniqueKey]) {
    const length = Buffer.alloc(8);
    length.writeBigUInt64BE(BigInt(Buffer.byteLength(part)));
    hash.update(length).update(part);
  }
  return `do/${hash.digest().subarray(0, 16).toString('base64url')}/${id}`;
}

// A port that was free a moment ago.
async function freePort() {
  const server = createServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address();
  await new Promise((resolve) => server.close(resolve));
  return port;
}

test('cluster: an instance bound to a wildcard advertises --cluster-advertise', async () => {
  const ports = [];
  const extraArgs = async (i) => {
    ports[i] = await freePort();
    return [
      `--cluster-addr=0.0.0.0:${ports[i]}`,
      `--cluster-advertise=127.0.0.1:${ports[i]}`,
    ];
  };
  await withCluster(
    2,
    async ({ nodes, bucket, nodeKeys }) => {
      for (const [i, node] of nodes.entries()) {
        const record = JSON.parse(
          await bucket.get(`nodes/${nodeKeys.get(node.nodeId)}.json`)
        );
        assert.strictEqual(record.address, `127.0.0.1:${ports[i]}`);
      }
      // Peers reach each other at the advertised addresses.
      const first = await fetchJson(
        nodes[0].httpPort,
        '/increment?name=advertised'
      );
      const second = await fetchJson(
        nodes[1].httpPort,
        '/increment?name=advertised'
      );
      assert.strictEqual(first.status, 200, JSON.stringify(first.body));
      assert.strictEqual(second.status, 200, JSON.stringify(second.body));
      assert.strictEqual(second.body.count, 2);
    },
    { extraArgs }
  );
});

// The status line a CONNECT to the cluster port at `port` gets, with `auth`
// as its cf-cluster-auth header, if any.
async function clusterConnect(port, auth) {
  const socket = connect(port, '127.0.0.1');
  await new Promise((resolve) => socket.once('connect', resolve));
  const header = auth === undefined ? '' : `cf-cluster-auth: ${auth}\r\n`;
  socket.write(
    `CONNECT cluster:1 HTTP/1.1\r\nhost: cluster:1\r\n${header}\r\n`
  );
  const [data] = await Promise.race([
    new Promise((resolve) => socket.once('data', (d) => resolve([d]))),
    sleep(5000).then(() => [Buffer.from('timeout')]),
  ]);
  socket.destroy();
  return data.toString().split('\r\n')[0];
}

// A cf-cluster-auth header for a CONNECT to `target`, as an instance with the
// test config's key makes it.
function clusterAuth(target, ms = Date.now(), nonce = randomBytes(16)) {
  const n = nonce.toString('hex');
  const mac = createHmac('sha256', 'test-cluster-shared-secret-of-32-bytes')
    .update(`workerd cluster connect v1\n${target}\n${ms}\n${n}`)
    .digest('hex');
  return `${ms}.${n}.${mac}`;
}

test('cluster: a CONNECT is refused when replayed, stale, misdirected or unsigned', async () => {
  const port = await freePort();
  const extraArgs = async () => [`--cluster-addr=127.0.0.1:${port}`];
  await withCluster(
    1,
    async () => {
      const target = `127.0.0.1:${port}`;
      const auth = clusterAuth(target);
      assert.match(await clusterConnect(port, auth), / 200 /);
      assert.match(await clusterConnect(port, auth), / 403 /, 'replayed');
      const stale = clusterAuth(target, Date.now() - 60_000);
      assert.match(await clusterConnect(port, stale), / 403 /, 'stale');
      const elsewhere = clusterAuth(`127.0.0.1:${port + 1}`);
      assert.match(await clusterConnect(port, elsewhere), / 403 /, 'target');
      assert.match(await clusterConnect(port), / 403 /, 'unsigned');
    },
    { extraArgs }
  );
});

test('cluster: a public cluster address is refused', async () => {
  const local = await mkdtemp(join(tmpdir(), 'workerd-cluster-'));
  for (const flag of ['--cluster-addr', '--cluster-advertise']) {
    const child = spawn(
      env.WORKERD_BINARY,
      [
        'serve',
        env.WD_TEST_CONFIG,
        '--experimental',
        '--socket-addr=http=127.0.0.1:0',
        `${flag}=8.8.8.8:7000`,
        `--directory-path=local=${local}`,
      ],
      { stdio: ['ignore', 'ignore', 'pipe'] }
    );
    let stderr = '';
    child.stderr.on('data', (d) => (stderr += d));
    const code = await new Promise((resolve) => child.once('exit', resolve));
    assert.notStrictEqual(code, 0, flag);
    assert.match(stderr, /8\.8\.8\.8 is public/, flag);
  }
  await rm(local, { recursive: true });
});

test('cluster: Durable Objects from before cluster mode are refused', async () => {
  await withCluster(1, async ({ bucket }) => {
    const local = await mkdtemp(join(tmpdir(), 'workerd-cluster-'));
    await mkdir(join(local, 'counter-test-namespace'));
    await writeFile(join(local, 'counter-test-namespace', 'a.sqlite'), '');
    const node = new ClusterNode({
      binary: env.WORKERD_BINARY,
      config: env.WD_TEST_CONFIG,
      localPath: local,
      s3Port: bucket.port,
      nodeId: 'old',
    });
    await assert.rejects(node.start(), /exited before listening/);
    assert.match(node.stderr, /from before cluster mode/);
    await rm(local, { recursive: true });
  });
});

test('cluster: each running instance holds a lease record', async () => {
  await withCluster(2, async ({ nodes, bucket, nodeKeys }) => {
    const records = await bucket.nodes();
    assert.deepStrictEqual(
      records.sort(),
      [...nodeKeys.values()].sort(),
      `expected one lease record per instance`
    );
    for (const key of records) {
      const record = JSON.parse(await bucket.get(`nodes/${key}.json`));
      assert(record.expires_ms > Date.now(), `lease of ${key} has expired`);
    }
  });
});

test('cluster: setting an alarm is refused', async () => {
  await withCluster(1, async ({ nodes }) => {
    // The handler catches the refusal: setAlarm() throws, not the commit.
    const res = await fetchJson(nodes[0].httpPort, '/set-alarm?name=alarm');
    assert.strictEqual(res.status, 200);
    assert.match(res.body.error ?? '', /not yet supported in cluster mode/);
  });
});

test('cluster: a DO reopened where it was closed resumes its epoch', async () => {
  await withCluster(1, async ({ nodes, bucket }) => {
    const [node] = nodes;
    await fetchJson(node.httpPort, '/increment?name=dormant');
    const before = await fetchJson(node.httpPort, '/increment?name=dormant');
    assert.strictEqual(before.body.count, 2, JSON.stringify(before.body));
    const id = before.body.id;
    // Breaking it drops its container, as expiry does: the database
    // closes, and the bucket gets what it lacks.
    await fetchJson(node.httpPort, '/break?name=dormant');
    await sleep(500);
    const after = await fetchJson(node.httpPort, '/increment?name=dormant');
    assert.strictEqual(after.body.count, 3, JSON.stringify(after.body));
    const prefix = objectPrefix(id);
    const record = JSON.parse(await bucket.get(`${prefix}/own.json`));
    assert.strictEqual(record.epoch, 1, JSON.stringify(record));
    // No session 2, whose name is `zd`.
    assert.deepStrictEqual(await bucket.list(`${prefix}/snapshot/zd/`), []);
    assert.match(node.stderr, /epoch 1 resumes at \d+/);
  });
});

test('cluster: storing and loading a stub claims nothing', async () => {
  await withCluster(2, async ({ nodes, bucket }) => {
    // DO "holder" (on whichever node claims it) stores a stub to DO "lazy" and
    // reads it back. Serializing and deserializing the stub must not claim
    // "lazy": no owner record, and no storage for it.
    const stored = await fetchJson(
      nodes[1].httpPort,
      '/store-stub?name=holder&target=lazy'
    );
    assert.strictEqual(stored.status, 200, JSON.stringify(stored.body));
    assert.strictEqual(stored.body.hasStub, true);
    const holderId = stored.body.id;
    const targetId = stored.body.targetId;
    assert.notStrictEqual(holderId, targetId);
    assert.notStrictEqual(
      await bucket.owner(holderId),
      null,
      `the holder DO itself must be claimed`
    );
    assert.strictEqual(
      await bucket.owner(targetId),
      null,
      `storing/loading a stub must not claim its target`
    );
    for (const node of nodes) {
      let entries = [];
      try {
        entries = await readdir(join(node.localPath, 'counter-test-namespace'));
      } catch (_) {
        // No storage at all on this node.
      }
      assert(
        !entries.some((name) => name.startsWith(targetId)),
        `storing/loading a stub must not create actor storage; found: ${entries.join(', ')}`
      );
    }

    // Calling the loaded stub claims it.
    const called = await fetchJson(
      nodes[1].httpPort,
      '/call-stored-stub?name=holder'
    );
    assert.strictEqual(called.status, 200, JSON.stringify(called.body));
    assert.strictEqual(called.body.target.id, targetId);
    assert.strictEqual(called.body.target.count, 0);
    assert.notStrictEqual(
      await bucket.owner(targetId),
      null,
      `calling the stub must claim its target`
    );
  });
});

// Requests that would hang forever if a broken actor kept its ownership lock
// are bounded by this timeout so the failure is a clear error.
const HANG_TIMEOUT_MS = 5000;

test('cluster: a broken DO pinned by a stub does not keep its ownership lock', async () => {
  await withCluster(2, async ({ nodes, bucket, nodeKeys }) => {
    // Node 0 claims "pinned". DO "holder", served by node 1, then obtains a
    // stub to it, uses it, and keeps it for as long as "holder" lives. Through
    // node 0's bootstrap for that stub, node 0's container for "pinned" stays
    // referenced for the rest of the test.
    const first = await fetchJson(nodes[0].httpPort, '/increment?name=pinned');
    assert.strictEqual(first.status, 200, JSON.stringify(first.body));
    assert.strictEqual(first.body.count, 1);
    const owner = first.body.nodeId;
    const objectId = first.body.id;
    assert.strictEqual(await bucket.owner(objectId), nodeKeys.get(owner));

    const held = await fetchJson(
      nodes[1].httpPort,
      '/hold-stub?name=holder&target=pinned'
    );
    assert.strictEqual(held.status, 200, JSON.stringify(held.body));
    assert.strictEqual(held.body.target.nodeId, owner);

    // Break the DO. Its container leaves the owner's map but lives on, pinned
    // by the parked stub.
    const broken = await fetchJson(nodes[1].httpPort, '/break?name=pinned');
    assert.strictEqual(broken.status, 500, JSON.stringify(broken.body));

    // The lock must have been released along with the map entry. Otherwise a
    // request from the non-owner routes to the owner, which finds no container
    // and routes to itself, forever.
    const other = nodes.find((n) => n.nodeId !== owner);
    const after = await fetchJson(other.httpPort, '/increment?name=pinned', {
      timeoutMs: HANG_TIMEOUT_MS,
    });
    assert.strictEqual(after.status, 200, JSON.stringify(after.body));
    assert.strictEqual(after.body.count, 2, 'state must survive the break');
    assert.strictEqual(
      await bucket.owner(objectId),
      nodeKeys.get(after.body.nodeId),
      'the lock file must name the node that re-instantiated the DO'
    );
  });
});

test('cluster: a DO whose constructor fails releases its ownership lock', async () => {
  await withCluster(2, async ({ nodes, bucket, nodeKeys }) => {
    const first = await fetchJson(nodes[0].httpPort, '/increment?name=ctor');
    assert.strictEqual(first.status, 200, JSON.stringify(first.body));
    const objectId = first.body.id;

    // Arm the one-shot constructor failure and abort the running instance so
    // the next request has to construct a new one.
    const armed = await fetchJson(
      nodes[1].httpPort,
      '/arm-constructor-failure?name=ctor'
    );
    assert.strictEqual(armed.status, 500, JSON.stringify(armed.body));

    // Re-instantiate through a stub kept by DO "holder". The constructor
    // throws, which breaks the new instance; the kept stub pins its hollowed
    // container on whichever node claimed the DO.
    const held = await fetchJson(
      nodes[1].httpPort,
      '/hold-stub?name=holder&target=ctor',
      { timeoutMs: HANG_TIMEOUT_MS }
    );
    assert.strictEqual(held.status, 500, JSON.stringify(held.body));
    assert.match(held.body.error, /constructor failed on purpose/);

    // A fresh request from the other node must not hang. The failed constructor
    // committed the cleared flag before throwing, so this instantiation
    // succeeds and sees the earlier increment.
    const after = await fetchJson(nodes[0].httpPort, '/increment?name=ctor', {
      timeoutMs: HANG_TIMEOUT_MS,
    });
    assert.strictEqual(after.status, 200, JSON.stringify(after.body));
    assert.strictEqual(after.body.count, 2);
    assert.strictEqual(
      await bucket.owner(objectId),
      nodeKeys.get(after.body.nodeId),
      'the lock file must name the node that re-instantiated the DO'
    );
  });
});

test('cluster: aborting a DO with a request in flight lets it be re-instantiated immediately', async () => {
  await withCluster(2, async ({ nodes, bucket, nodeKeys }) => {
    const first = await fetchJson(
      nodes[0].httpPort,
      '/increment?name=inflight'
    );
    assert.strictEqual(first.status, 200, JSON.stringify(first.body));
    const owner = first.body.nodeId;
    const objectId = first.body.id;

    // Start a request the DO holds open, give it time to reach the DO, then
    // abort the DO underneath it. The in-flight request keeps a reference to
    // the old Worker::Actor until it unwinds, but the storage must be closed
    // and the lock released as soon as the actor breaks, not when that
    // reference goes away.
    const holding = fetchJson(nodes[0].httpPort, '/hold?name=inflight', {
      timeoutMs: HANG_TIMEOUT_MS,
    });
    await sleep(250);
    const broken = await fetchJson(nodes[0].httpPort, '/break?name=inflight');
    assert.strictEqual(broken.status, 500, JSON.stringify(broken.body));

    const other = nodes.find((n) => n.nodeId !== owner);
    const after = await fetchJson(other.httpPort, '/increment?name=inflight', {
      timeoutMs: HANG_TIMEOUT_MS,
    });
    assert.strictEqual(after.status, 200, JSON.stringify(after.body));
    assert.strictEqual(
      after.body.count,
      2,
      'the new instance must see the state committed before the abort'
    );
    assert.strictEqual(
      await bucket.owner(objectId),
      nodeKeys.get(after.body.nodeId),
      'the lock file must name the node that re-instantiated the DO'
    );

    const held = await holding;
    assert.strictEqual(held.status, 500, JSON.stringify(held.body));
  });
});

// Sets up the cross-node persistent-stub scenario of the test below: DO "root"
// is claimed by node 0, then DO "holder", served through node 1 (and therefore
// claimed by node 1), asks "root" to vend a persistent stub of the given kind
// and stores it. Returns root's node ID.
async function vendAcrossNodes(nodes, kind) {
  const first = await fetchJson(nodes[0].httpPort, '/increment?name=root');
  assert.strictEqual(first.status, 200, JSON.stringify(first.body));
  const rootOwner = first.body.nodeId;

  const vended = await fetchJson(
    nodes[1].httpPort,
    `/vend-stub?name=holder&kind=${kind}&target=root`,
    { timeoutMs: HANG_TIMEOUT_MS }
  );
  assert.strictEqual(vended.status, 200, JSON.stringify(vended.body));
  assert.strictEqual(vended.body.hasStub, true);
  assert.notStrictEqual(
    vended.body.nodeId,
    rootOwner,
    'the test requires root and holder to live on different nodes'
  );

  // Vending ran [restore]() live. Storing and loading the stub must not have
  // replayed it.
  const state = await fetchJson(nodes[1].httpPort, '/get?name=root');
  assert.strictEqual(state.status, 200, JSON.stringify(state.body));
  assert.strictEqual(state.body.restores, 1);

  return rootOwner;
}

// TODO(cluster): the token-carrying peer call (bucket-mvp's getChannelFromToken).
test(
  'cluster: a restored RpcTarget stub is redeemed on the root DO owner',
  { todo: true },
  async () => {
    await withCluster(2, async ({ nodes }) => {
      const rootOwner = await vendAcrossNodes(nodes, 'rpc');

      // The RpcTarget mutates root's own storage, so it must run in root's
      // context on root's owner.
      const used = await fetchJson(
        nodes[1].httpPort,
        '/use-stub?name=holder&kind=rpc&amount=10',
        { timeoutMs: HANG_TIMEOUT_MS }
      );
      assert.strictEqual(used.status, 200, JSON.stringify(used.body));
      assert.deepStrictEqual(used.body.result, {
        count: 11,
        nodeId: rootOwner,
      });

      const state = await fetchJson(nodes[0].httpPort, '/get?name=root');
      assert.strictEqual(state.body.count, 11);
      assert.strictEqual(state.body.restores, 2);
    });
  }
);

test('cluster: a stub resolved to a remote DO keeps failing after that DO breaks', async () => {
  await withCluster(2, async ({ nodes }) => {
    const first = await fetchJson(nodes[0].httpPort, '/increment?name=sticky');
    assert.strictEqual(first.status, 200, JSON.stringify(first.body));
    const owner = first.body.nodeId;

    // DO "holder" on node 1 obtains a stub to "sticky", uses it once (resolving
    // it to node 0's instance), and keeps it.
    const held = await fetchJson(
      nodes[1].httpPort,
      '/hold-stub?name=holder&target=sticky'
    );
    assert.strictEqual(held.status, 200, JSON.stringify(held.body));
    assert.strictEqual(held.body.target.nodeId, owner);
    assert.notStrictEqual(held.body.nodeId, owner);

    const broken = await fetchJson(nodes[1].httpPort, '/break?name=sticky');
    assert.strictEqual(broken.status, 500, JSON.stringify(broken.body));

    // The held stub points at the broken instance and must keep failing, even
    // after a fresh stub has re-instantiated the DO.
    const heldAfter = await fetchJson(
      nodes[1].httpPort,
      '/call-held-stub?name=holder&target=sticky',
      { timeoutMs: HANG_TIMEOUT_MS }
    );
    assert.strictEqual(heldAfter.status, 500, JSON.stringify(heldAfter.body));
    assert.match(heldAfter.body.error, /broken on purpose/);

    const fresh = await fetchJson(nodes[1].httpPort, '/increment?name=sticky', {
      timeoutMs: HANG_TIMEOUT_MS,
    });
    assert.strictEqual(fresh.status, 200, JSON.stringify(fresh.body));
    assert.strictEqual(fresh.body.count, 2);

    const heldAgain = await fetchJson(
      nodes[1].httpPort,
      '/call-held-stub?name=holder&target=sticky',
      { timeoutMs: HANG_TIMEOUT_MS }
    );
    assert.strictEqual(heldAgain.status, 500, JSON.stringify(heldAgain.body));
    assert.match(heldAgain.body.error, /broken on purpose/);
  });
});

test('cluster: a freshly minted RpcTarget stub passed via props reaches its DO after it breaks', async () => {
  await withCluster(2, async ({ nodes }) => {
    const first = await fetchJson(nodes[0].httpPort, '/increment?name=root');
    assert.strictEqual(first.status, 200, JSON.stringify(first.body));
    const originalOwner = first.body.nodeId;

    // Root mints an RpcTarget stub and hands it to a Consumer entrypoint on the
    // same node via props. The Consumer holds the stub's channel only -- the
    // very channel object ctx.restore() created, not a decoded token -- and
    // waits for the go signal before using it.
    const vended = await fetchJson(
      nodes[0].httpPort,
      '/vend-to-consumer?name=root'
    );
    assert.strictEqual(vended.status, 200, JSON.stringify(vended.body));

    // Break root, then re-instantiate it through the other node. A broken DO
    // keeps its owner, which re-instantiates it.
    const broken = await fetchJson(nodes[1].httpPort, '/break?name=root');
    assert.strictEqual(broken.status, 500, JSON.stringify(broken.body));
    const moved = await fetchJson(nodes[1].httpPort, '/increment?name=root', {
      timeoutMs: HANG_TIMEOUT_MS,
    });
    assert.strictEqual(moved.status, 200, JSON.stringify(moved.body));
    assert.strictEqual(moved.body.count, 2);
    assert.strictEqual(moved.body.nodeId, originalOwner);

    // Now let the Consumer use its stub: it reaches the re-instantiated DO.
    const go = await fetchJson(nodes[0].httpPort, '/set-go?name=sink');
    assert.strictEqual(go.status, 200, JSON.stringify(go.body));

    const deadline = Date.now() + HANG_TIMEOUT_MS;
    let recorded;
    while (recorded === undefined) {
      assert(Date.now() < deadline, 'Consumer never recorded a result');
      await sleep(50);
      const sink = await fetchJson(nodes[0].httpPort, '/get?name=sink');
      recorded = sink.body.recorded;
    }
    assert.deepStrictEqual(recorded, { count: 3, nodeId: moved.body.nodeId });

    const state = await fetchJson(nodes[1].httpPort, '/get?name=root');
    assert.strictEqual(state.body.restores, 2);
  });
});
