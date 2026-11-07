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
//      breaks, while a fresh stub reaches the re-instantiated DO. One whose
//      owner drains follows the DO to its next owner, also once that owner
//      exited, and a request that may have run on the drained owner is not
//      replayed there.
//   9. Facets and their deletions survive a takeover.
//  10. Draining cancels a running onHibernate handler.
//  11. A fresh cluster serves its bucket's deployment, Durable Objects
//      included. A node serves its own services and usable deployments when
//      another deployment has a broken service or is skipped.
//      `workerd deploy` publishes a Cap'n Proto config or a wrangler project for
//      the nodes to start on, and refuses a deployment that holds more than
//      services or conflicts with another.
//  12. A node stopping under load hands its DOs to its peers, two at once
//      too: requests that arrive meanwhile wait for a DO's handoff and follow
//      it, JS RPC calls and other DOs' calls included, the calls of a
//      handoff's work are not held, and a DO whose handoff outlasts its
//      deadline moves once the node left. A rolling restart of the cluster,
//      paced by the nodes' readiness, fails, loses and replays no request.
//  13. KV: a namespace is seen from every node, its large values are in the
//      bucket and cached on each node's disk, and it survives its owner.
//
// A takeover waits out a dead owner's 20 s lease, so each one here costs
// that much: one test per mechanism. The simulations in ../../cluster
// (alarms-test.rs, capture-test.rs, owner-test.rs) search the cases.
// These run there, not here, so not through a real binary and SQLite
// (`git log -S'facet clones survive'` has the tests that did):
//   - facet clones, nested deletions and deleteAll() across a takeover:
//     owner-test.rs, capture-test.rs;
//   - a dead owner's alarms adopted with its node record gone: only the
//     orphan scan finds them. alarms-test.rs.

import { spawn } from 'node:child_process';
import { mkdir, mkdtemp, readdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { connect } from 'node:net';
import { createHash, createHmac, randomBytes } from 'node:crypto';
import { join } from 'node:path';
import { env } from 'node:process';
import { test as nodeTest } from 'node:test';
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

// Bazel shards this file (`shard_count`): each shard runs every n-th test.
const SHARDS = Number(env.TEST_TOTAL_SHARDS ?? 1);
const SHARD = Number(env.TEST_SHARD_INDEX ?? 0);
if (env.TEST_SHARD_STATUS_FILE !== undefined) {
  await writeFile(env.TEST_SHARD_STATUS_FILE, '');
}
let testIndex = 0;
function test(...args) {
  if (testIndex++ % SHARDS === SHARD) nodeTest(...args);
}

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
  #clusterPort = null;
  #closed = null;
  #stderrBuffer = '';
  #extraArgs;
  #disk;
  #configName;

  // `localPath` is the directory of the config's disk service `disk`.
  constructor({
    binary,
    config,
    configName,
    localPath,
    disk = 'local',
    s3Port,
    nodeId,
    extraArgs = [],
  }) {
    this.#extraArgs = extraArgs;
    this.#disk = disk;
    this.#configName = configName;
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

  // The port the node's cluster peer port is bound to.
  get clusterPort() {
    assert(this.#clusterPort !== null, 'node has no cluster port');
    return this.#clusterPort;
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
      ...(this.#configName === undefined ? [] : [this.#configName]),
      '--experimental',
      '--verbose',
      `--control-fd=${CONTROL_FD}`,
      `--socket-addr=http=127.0.0.1:0`,
    ];
    if (this.#localPath !== undefined) {
      args.push(`--directory-path=${this.#disk}=${this.#localPath}`);
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

    // Watch the control fd for the assigned ports. The cluster port's event,
    // if any, comes before the sockets'.
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
            if (parsed.event === 'listen-cluster') {
              this.#clusterPort = parsed.port;
            }
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
    this.#clusterPort = null;
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

  async delete(key) {
    await fetch(`http://127.0.0.1:${this.port}/wdtest/cluster/v1/${key}`, {
      method: 'DELETE',
    });
  }

  async put(key, body, headers = {}) {
    const res = await fetch(
      `http://127.0.0.1:${this.port}/wdtest/cluster/v1/${key}`,
      { method: 'PUT', body, headers }
    );
    assert.strictEqual(res.status, 200, `PUT ${key} answered ${res.status}`);
  }

  // The key of the node that owns the Counter with this ID, or null when no
  // owner record exists.
  async owner(id) {
    const record = await this.get(`${objectPrefix(id)}/own.json`);
    return record === null ? null : JSON.parse(record).node;
  }

  // Arms `faults` (fake-s3.js), each `prefix` a key in the cluster's layout,
  // replacing those armed before.
  async setFaults(faults) {
    const armed = faults.map((f) => ({
      ...f,
      prefix: `cluster/v1/${f.prefix}`,
    }));
    const res = await fetch(`http://127.0.0.1:${this.port}/_faults`, {
      method: 'POST',
      body: JSON.stringify(armed),
    });
    await res.arrayBuffer();
  }

  // How many requests each armed fault has matched.
  async faultHits() {
    const res = await fetch(`http://127.0.0.1:${this.port}/_faults`);
    return (await res.json()).map((f) => f.hits);
  }

  // How many GET, PUT and DELETE requests the bucket served for `key`.
  async requests(key) {
    const query = new URLSearchParams({ key });
    return (
      await fetch(`http://127.0.0.1:${this.port}/_requests?${query}`)
    ).json();
  }

  // How many body bytes GETs of `key` were served.
  async served(key) {
    const query = new URLSearchParams({ key });
    return (
      await fetch(`http://127.0.0.1:${this.port}/_served?${query}`)
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
// The nodes run the config constant `configName` of `config`, whose disk
// service `disk` is a directory of each node's own, once `before` has prepared
// the bucket.
async function withCluster(
  numNodes,
  fn,
  {
    extraArgs = () => [],
    config = env.WD_TEST_CONFIG,
    configName = 'config',
    disk = 'local',
    before = async () => {},
  } = {}
) {
  // Each test run gets its own bucket and disks, so state doesn't leak
  // between tests.
  const root = await mkdtemp(join(tmpdir(), 'workerd-cluster-'));
  const bucket = new FakeS3();
  const nodes = [];
  const nodeKeys = new Map();
  try {
    await bucket.start();
    await before(bucket);
    for (let i = 0; i < numNodes; i++) {
      const localPath = join(root, `node${i}`);
      await mkdir(localPath);
      const node = new ClusterNode({
        binary: env.WORKERD_BINARY,
        config,
        configName,
        localPath,
        disk,
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
    // Kill all nodes, some of which the test may have stopped already: a
    // drain, which keeps accepting requests for seconds, is for the tests
    // that check it to wait out.
    await Promise.all(
      nodes.map((node) =>
        node.stop({ signal: 'SIGKILL', timeoutMs: 5000 }).catch(() => {})
      )
    );
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

    // Facets and a deletion ride along: real SQLite, restored elsewhere.
    for (const path of ['a', 'a', 'a,b%2Fc', 'x', 'x,y']) {
      await facetAdd(nodes[0].httpPort, 'takeover', path);
    }
    // Nothing commits after the delete: it is durable once it returns.
    const del = await fetchJson(
      nodes[0].httpPort,
      '/facet-delete?name=takeover&path=x'
    );
    assert.strictEqual(del.status, 200, JSON.stringify(del.body));

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

    const p = survivor.httpPort;
    assert.strictEqual(await facetAdd(p, 'takeover', 'a'), 3);
    assert.strictEqual(await facetAdd(p, 'takeover', 'a,b%2Fc'), 2);
    assert.strictEqual(await facetAdd(p, 'takeover', 'x'), 1);
    assert.strictEqual(await facetAdd(p, 'takeover', 'x,y'), 1);
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
  'cluster: a draining node sends a request for an unowned DO to a peer',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(2, async ({ nodes, bucket, nodeKeys }) => {
      // Put a request in flight on node 0 for a DO that nobody owns yet, then
      // SIGTERM node 0 while its stateless handler is still waiting. By the time
      // the handler calls the DO, node 0 is draining: claiming the DO there
      // would only hand it off again, so its peer claims it instead.
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
      assert.strictEqual(response.status, 200, JSON.stringify(response.body));
      assert.strictEqual(response.body.count, 1);
      assert.strictEqual(response.body.nodeId, nodes[1].nodeId);

      const exit = await exitPromise;
      assert.deepStrictEqual(exit, { code: 0, signal: null });

      const survivor = await fetchJson(
        nodes[1].httpPort,
        '/increment?name=lame-duck'
      );
      assert.strictEqual(survivor.status, 200, JSON.stringify(survivor.body));
      assert.strictEqual(survivor.body.count, 2);
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

      // A slow request keeps the owner draining for longer than its lease
      // would take to expire.
      const holding = fetchJson(
        owner.httpPort,
        '/increment?name=hold&delay=12000',
        { timeoutMs: 20_000 }
      );
      await sleep(250);
      const drainedAt = Date.now();
      const exitPromise = owner.stop({ signal: 'SIGTERM', timeoutMs: 20_000 });

      // The owner hands the idle DO off as soon as it drains: the peer claims
      // it at once, not once the owner left.
      const response = await fetchJson(
        peer.httpPort,
        '/increment?name=handoff',
        { timeoutMs: 5_000 }
      );
      assert.strictEqual(response.status, 200, JSON.stringify(response.body));
      assert.strictEqual(response.body.nodeId, peer.nodeId);
      assert.strictEqual(response.body.count, 2);
      assert(Date.now() - drainedAt < 5_000);
      await holding.catch(() => {});
      assert.deepStrictEqual(await exitPromise, { code: 0, signal: null });
    });
  }
);

test(
  'cluster: draining cancels a running onHibernate handler',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(1, async ({ nodes: [node] }) => {
      const url = (path) =>
        `http://127.0.0.1:${node.httpPort}/on-hibernate/${path}`;
      assert.strictEqual(await (await fetch(url('start'))).text(), 'running');

      // The eviction waits for the handler, which outlasts its 10 s budget.
      const eviction = fetch(url('evict'), {
        signal: AbortSignal.timeout(20_000),
      });
      const deadline = Date.now() + 10_000;
      while (!(await (await fetch(url('started'))).json())) {
        assert(Date.now() < deadline, 'the onHibernate handler did not start');
        await sleep(50);
      }

      // Releasing the object's ownership cancels the handler, so the waiting
      // eviction fails at once and the drain doesn't wait out the budget. The
      // drain itself still keeps accepting requests for its grace period.
      const drainedAt = Date.now();
      const evicted = eviction.then((response) => ({
        status: response.status,
        ms: Date.now() - drainedAt,
      }));
      const exit = await node.stop({ signal: 'SIGTERM', timeoutMs: 20_000 });
      const drainMs = Date.now() - drainedAt;
      assert.deepStrictEqual(exit, { code: 0, signal: null });
      assert(drainMs < 10_000, `the drain took ${drainMs} ms`);
      const { status, ms } = await evicted;
      assert.strictEqual(status, 500);
      assert(ms < 5_000, `the eviction failed after ${ms} ms`);
    });
  }
);

test(
  'cluster: a DO whose node drains while restoring it does not start there',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(3, async ({ nodes, bucket }) => {
      const first = await fetchJson(
        nodes[0].httpPort,
        '/increment?name=restoring'
      );
      assert.strictEqual(first.body.nodeId, nodes[0].nodeId);
      const exit0 = await nodes[0].stop({
        signal: 'SIGTERM',
        timeoutMs: 10_000,
      });
      assert.deepStrictEqual(exit0, { code: 0, signal: null });

      // Node 1 takes the DO over; its reads of the DO's log wait, so that it
      // is still restoring the DO when it drains.
      await bucket.setFaults([
        {
          prefix: `${objectPrefix(first.body.id)}/`,
          except: '/own.json',
          method: 'GET',
          delayMs: 3000,
          times: 1000,
        },
      ]);
      const request = fetchJson(
        nodes[1].httpPort,
        '/increment?name=restoring',
        {
          timeoutMs: 20_000,
        }
      ).catch((err) => ({ status: null, body: { error: String(err) } }));
      const deadline = Date.now() + 10_000;
      while ((await bucket.faultHits())[0] === 0) {
        assert(Date.now() < deadline, 'node 1 never restored the DO');
        await sleep(50);
      }
      const exit1 = nodes[1].stop({ signal: 'SIGTERM', timeoutMs: 20_000 });
      const response = await request;
      // Node 1 hands the DO off rather than start it, so the request either
      // fails unsent or is sent on to the node that claims the DO next.
      assert.notStrictEqual(
        response.body.nodeId,
        nodes[1].nodeId,
        `the DO started on the drained node: ${JSON.stringify(response.body)}`
      );
      const served = response.status === 200 ? 1 : 0;
      await bucket.setFaults([]);
      assert.deepStrictEqual(await exit1, { code: 0, signal: null });

      let after = null;
      const until = Date.now() + 15_000;
      while (Date.now() < until) {
        after = await fetchJson(
          nodes[2].httpPort,
          '/increment?name=restoring',
          {
            timeoutMs: 15_000,
          }
        ).catch(() => null);
        if (after?.status === 200) break;
        await sleep(200);
      }
      assert.strictEqual(after?.status, 200, JSON.stringify(after?.body));
      assert.strictEqual(after.body.nodeId, nodes[2].nodeId);
      assert.strictEqual(after.body.count, 2 + served);
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

test('cluster: a KV namespace is an object, seen from every node', async () => {
  await withCluster(2, async ({ nodes, bucket }) => {
    const [a, b] = nodes.map((n) => n.httpPort);
    const q = (params) => new URLSearchParams(params);
    for (const key of ['p/1', 'p/2', 'p/3', 'q']) {
      const put = await fetchJson(a, `/kv/put?${q({ key, value: key })}`);
      assert.strictEqual(put.status, 200, JSON.stringify(put.body));
    }
    const meta = { key: 'm', value: 'v', metadata: '{"n":1}', ttl: '3600' };
    await fetchJson(a, `/kv/put?${q(meta)}`);

    const got = await fetchJson(b, `/kv/get?${q({ key: 'm' })}`);
    assert.deepStrictEqual(got.body, {
      value: 'v',
      metadata: { n: 1 },
      cacheStatus: 'MISS',
    });

    const names = [];
    let page = { list_complete: false };
    while (!page.list_complete) {
      const query = { prefix: 'p/', limit: '2', cursor: page.cursor ?? '' };
      page = (await fetchJson(b, `/kv/list?${q(query)}`)).body;
      names.push(...page.keys.map((k) => k.name));
    }
    assert.deepStrictEqual(names, ['p/1', 'p/2', 'p/3']);
    const listed = (await fetchJson(b, `/kv/list?${q({ prefix: 'm' })}`)).body;
    assert.deepStrictEqual(listed.keys[0].metadata, { n: 1 });
    assert.ok(listed.keys[0].expiration > Date.now() / 1000 + 3500);

    await fetchJson(b, `/kv/delete?${q({ key: 'q' })}`);
    const gone = await fetchJson(b, `/kv/get?${q({ key: 'q' })}`);
    assert.strictEqual(gone.body.value, null);
    // Small values are inline in the object: none is in the bucket.
    assert.deepStrictEqual(await bucket.list('services/kv/'), []);

    // A large value is a blob, and a listing does not read it.
    for (const size of ['65536', '65537']) {
      const big = { key: `big/${size}`, size, metadata: '{"b":1}' };
      await fetchJson(a, `/kv/put?${q(big)}`);
    }
    const blobs = await bucket.list('services/kv/');
    assert.strictEqual(blobs.length, 1, blobs.join());
    const served = () => bucket.served(`v1/${blobs[0]}`);
    const before = await served();
    const bigListed = (await fetchJson(b, `/kv/list?${q({ prefix: 'big' })}`))
      .body;
    assert.deepStrictEqual(
      bigListed.keys.map((k) => k.name),
      ['big/65536', 'big/65537']
    );
    assert.strictEqual(await served(), before);
    const value = await fetchJson(b, `/kv/get?${q({ key: 'big/65537' })}`);
    assert.strictEqual(value.body.value.length, 65537);
    assert.strictEqual(await served(), before + 65537);
  });
});

// A node keeps the KV blobs it writes and reads on its disk, so it
// downloads a value once.
test('cluster: a node caches KV blobs on its disk', async () => {
  await withCluster(2, async ({ nodes, bucket }) => {
    const [a, b] = nodes.map((n) => n.httpPort);
    const q = (params) => new URLSearchParams(params);
    const MiB = 1024 * 1024;
    const put = (port, key, size) =>
      fetchJson(port, `/kv/put?${q({ key, size: String(size) })}`);
    const get = async (port, key) =>
      (await fetchJson(port, `/kv/get?${q({ key })}`)).body.value.length;
    // Bytes the bucket served of every blob it has held.
    const seen = new Set();
    const served = async () => {
      for (const key of await bucket.list('services/kv/')) seen.add(key);
      let n = 0;
      for (const key of seen) n += await bucket.served(`v1/${key}`);
      return n;
    };

    await put(a, 'k1', 1.5 * MiB);
    const before = await served();
    // The writer wrote it through; another node downloads it once.
    assert.strictEqual(await get(a, 'k1'), 1.5 * MiB);
    assert.strictEqual(await served(), before);
    for (let i = 0; i < 2; i++) {
      assert.strictEqual(await get(b, 'k1'), 1.5 * MiB);
      assert.strictEqual(await served(), before + 1.5 * MiB);
    }

    // An overwrite is a new blob.
    await put(a, 'k1', 1.25 * MiB);
    assert.strictEqual(await get(a, 'k1'), 1.25 * MiB);
    assert.strictEqual(await served(), before + 1.5 * MiB);
  });
});

// A read whose reply arrives after a newer write from the same node does
// not replace the write's entry in the node's cache.
test('cluster: a KV read racing a write keeps the write cached', async () => {
  await withCluster(1, async ({ nodes }) => {
    const [a] = nodes.map((n) => n.httpPort);
    const q = (params) => new URLSearchParams(params);
    const size = String(2 * 1024 * 1024);
    await fetchJson(a, `/kv/put?${q({ key: 'race', size })}`);
    await Promise.all([
      fetchJson(a, `/kv/get?${q({ key: 'race' })}`),
      fetchJson(a, `/kv/put?${q({ key: 'race', value: 'new' })}`),
    ]);
    const now = (await fetchJson(a, `/kv/get?${q({ key: 'race' })}`)).body;
    assert.deepStrictEqual([now.value, now.cacheStatus], ['new', 'HIT']);
  });
});

// A node's cache keeps what it read for the read's cacheTtl; another
// node's write does not reach it sooner.
test('cluster: KV reads are stale for at most cacheTtl', async () => {
  await withCluster(2, async ({ nodes }) => {
    const [a, b] = nodes.map((n) => n.httpPort);
    const q = (params) => new URLSearchParams(params);
    const read = async (port) =>
      (await fetchJson(port, `/kv/get?${q({ key: 's', cacheTtl: '30' })}`))
        .body;
    await fetchJson(b, `/kv/put?${q({ key: 's', value: 'old' })}`);
    assert.strictEqual((await read(a)).value, 'old');
    await fetchJson(b, `/kv/put?${q({ key: 's', value: 'new' })}`);
    assert.strictEqual((await read(b)).value, 'new');
    const stale = await read(a);
    assert.deepStrictEqual([stale.value, stale.cacheStatus], ['old', 'HIT']);
    await fetchJson(a, `/kv/clock?${q({ advance: '31000' })}`);
    assert.strictEqual((await read(a)).value, 'new');
  });
});

// KV fails over as any object does: once a namespace's owner dies, the
// survivor reads its large values from the bucket, expires its keys, and
// collects the blobs it replaces.
test('cluster: a KV namespace survives its owner', async () => {
  await withCluster(2, async ({ nodes, bucket, nodeKeys }) => {
    const kv = (node, op, params) =>
      fetchJson(node.httpPort, `/kv/${op}?${new URLSearchParams(params)}`, {
        timeoutMs: 10_000,
      });
    await kv(nodes[0], 'put', { key: 'big', size: '100000' });
    await kv(nodes[0], 'put', { key: 'ttl', value: 'v', ttl: '60' });
    const records = await bucket.list(objectPrefix('', 'kv'));
    const own = records.find((key) => key.endsWith('/own.json'));
    const ownerKey = JSON.parse(await bucket.get(own)).node;
    const owner = nodes.find((n) => nodeKeys.get(n.nodeId) === ownerKey);
    const survivor = nodes.find((n) => n !== owner);
    await owner.stop({ signal: 'SIGKILL', timeoutMs: 2000 });

    // Requests fail until the dead owner's lease expires.
    let got = null;
    const deadline = Date.now() + 30_000;
    while (Date.now() < deadline) {
      got = await kv(survivor, 'get', { key: 'big' }).catch(() => null);
      if (got?.status === 200) break;
      await sleep(200);
    }
    assert.strictEqual(got?.status, 200, JSON.stringify(got?.body));
    assert.strictEqual(got.body.value.length, 100000);

    assert.strictEqual(
      (await kv(survivor, 'get', { key: 'ttl' })).body.value,
      'v'
    );
    await kv(survivor, 'clock', { advance: '61000' });
    assert.strictEqual(
      (await kv(survivor, 'get', { key: 'ttl' })).body.value,
      null
    );
    const listed = await kv(survivor, 'list', { prefix: '' });
    assert.deepStrictEqual(
      listed.body.keys.map((k) => k.name),
      ['big']
    );

    assert.strictEqual((await bucket.list('services/kv/')).length, 1);
    await kv(survivor, 'put', { key: 'big', value: 'small' });
    const collected = Date.now() + 10_000;
    while ((await bucket.list('services/kv/')).length > 0) {
      assert(Date.now() < collected, 'the replaced blob was never collected');
      await sleep(100);
    }
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

test('cluster: an instance bound to a wildcard advertises --cluster-advertise', async () => {
  // Port 0 for both: the advertised address takes the port the kernel chose.
  const extraArgs = () => [
    '--cluster-addr=0.0.0.0:0',
    '--cluster-advertise=127.0.0.1:0',
  ];
  await withCluster(
    2,
    async ({ nodes, bucket, nodeKeys }) => {
      for (const node of nodes) {
        const record = JSON.parse(
          await bucket.get(`nodes/${nodeKeys.get(node.nodeId)}.json`)
        );
        assert.notStrictEqual(node.clusterPort, 0);
        assert.strictEqual(record.address, `127.0.0.1:${node.clusterPort}`);
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
  await withCluster(1, async ({ nodes }) => {
    const port = nodes[0].clusterPort;
    const target = `127.0.0.1:${port}`;
    const auth = clusterAuth(target);
    assert.match(await clusterConnect(port, auth), / 200 /);
    assert.match(await clusterConnect(port, auth), / 403 /, 'replayed');
    const stale = clusterAuth(target, Date.now() - 60_000);
    assert.match(await clusterConnect(port, stale), / 403 /, 'stale');
    const elsewhere = clusterAuth(`127.0.0.1:${port + 1}`);
    assert.match(await clusterConnect(port, elsewhere), / 403 /, 'target');
    assert.match(await clusterConnect(port), / 403 /, 'unsigned');
  });
});

test('cluster: a public cluster address is refused', async () => {
  const local = await mkdtemp(join(tmpdir(), 'workerd-cluster-'));
  // The unreachable bucket proves that address validation runs before
  // bucket access.
  for (const flag of ['--cluster-addr', '--cluster-advertise']) {
    const child = spawn(
      env.WORKERD_BINARY,
      [
        'serve',
        env.WD_TEST_CONFIG,
        'config',
        '--experimental',
        '--socket-addr=http=127.0.0.1:0',
        `${flag}=8.8.8.8:7000`,
        `--directory-path=local=${local}`,
      ],
      {
        stdio: ['ignore', 'ignore', 'pipe'],
        env: { ...env, AWS_ENDPOINT_URL: 'http://127.0.0.1:1' },
      }
    );
    let stderr = '';
    child.stderr.on('data', (d) => (stderr += d));
    const code = await new Promise((resolve) => child.once('exit', resolve));
    assert.notStrictEqual(code, 0, flag);
    assert.match(stderr, /8\.8\.8\.8 is public/, flag);
    assert.doesNotMatch(stderr, /could not load deployments/, flag);
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
      configName: 'config',
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

test("cluster: an alarm runs once on its object's owner", async () => {
  await withCluster(2, async ({ nodes }) => {
    const set = await fetchJson(
      nodes[0].httpPort,
      '/set-alarm?name=alarm&ms=500'
    );
    assert.strictEqual(set.status, 200, JSON.stringify(set.body));
    await sleep(3000);
    const res = await fetchJson(nodes[1].httpPort, '/alarms?name=alarm');
    assert.deepStrictEqual(res.body.fired, [set.body.nodeId]);
  });
});

test("cluster: a killed owner's alarm runs once on a survivor", async () => {
  await withCluster(2, async ({ nodes, bucket, nodeKeys }) => {
    const set = await fetchJson(
      nodes[0].httpPort,
      '/set-alarm?name=orphan&ms=5000'
    );
    assert.strictEqual(set.status, 200, JSON.stringify(set.body));
    const ownerIndex = nodes.findIndex((n) => n.nodeId === set.body.nodeId);
    const survivor = nodes[1 - ownerIndex];
    await nodes[ownerIndex].stop({ signal: 'SIGKILL', timeoutMs: 2000 });
    // No request touches the object: the survivor finds the alarm in the
    // dead owner's index once its lease lapses, and claims the object when
    // the alarm fires. Watch the bucket for that claim, then the alarm.
    const deadline = Date.now() + 30_000;
    let res;
    for (;;) {
      if ((await bucket.owner(set.body.id)) === nodeKeys.get(survivor.nodeId)) {
        res = await fetchJson(survivor.httpPort, '/alarms?name=orphan', {
          timeoutMs: 10_000,
        });
        if (res.body.fired.length > 0) break;
      }
      assert(Date.now() < deadline, `${survivor.nodeId} never ran the alarm`);
      await sleep(200);
    }
    assert.strictEqual(res.body.nodeId, survivor.nodeId);
    assert.deepStrictEqual(res.body.fired, [survivor.nodeId]);
  });
});

async function facetAdd(port, name, path) {
  const r = await fetchJson(port, `/facet-add?name=${name}&path=${path}`);
  assert.strictEqual(r.status, 200, JSON.stringify(r.body));
  return r.body.count;
}

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

// Node 0 claims each of `targets`, and DO "holder", served by node 1, holds a
// stub to each, resolved to node 0. Then node 0 drains, kept draining by a
// slow request that it routes to node 1 in the end, and is gone from the
// bucket's listing when this returns.
async function holdThenDrain(nodes, bucket, nodeKeys, targets) {
  for (const target of targets) {
    const first = await fetchJson(
      nodes[0].httpPort,
      `/increment?name=${target}`
    );
    assert.strictEqual(first.body.nodeId, nodes[0].nodeId);
    const held = await fetchJson(
      nodes[1].httpPort,
      `/hold-stub?name=holder&target=${target}`
    );
    assert.strictEqual(held.status, 200, JSON.stringify(held.body));
    assert.strictEqual(held.body.nodeId, nodes[1].nodeId);
    assert.strictEqual(held.body.target.nodeId, nodes[0].nodeId);
  }
  const holding = fetchJson(
    nodes[0].httpPort,
    '/increment?name=holder&delay=4000',
    { timeoutMs: 10_000 }
  ).catch(() => {});
  await sleep(250);
  const exit = nodes[0].stop({ signal: 'SIGTERM', timeoutMs: 10_000 });
  const deadline = Date.now() + 5000;
  while ((await bucket.nodes()).includes(nodeKeys.get(nodes[0].nodeId))) {
    assert(Date.now() < deadline, 'the draining node never left');
    await sleep(50);
  }
  return { holding, exit };
}

test(
  'cluster: a held stub follows its DO once the owner drains',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(2, async ({ nodes, bucket, nodeKeys }) => {
      const { holding, exit } = await holdThenDrain(nodes, bucket, nodeKeys, [
        'follow',
        'follow-rpc',
      ]);

      // The drained owner sends the fetch on to the node that claims the DO
      // now, or answers that the DO moved, before anything ran, so that it is
      // sent there again.
      const fetched = await fetchJson(
        nodes[1].httpPort,
        '/call-held-stub?name=holder&target=follow&path=/increment',
        { timeoutMs: HANG_TIMEOUT_MS }
      );
      assert.strictEqual(fetched.status, 200, JSON.stringify(fetched.body));
      assert.strictEqual(fetched.body.target.nodeId, nodes[1].nodeId);
      assert.strictEqual(fetched.body.target.count, 2);

      // A JS RPC call is not sent again. One that reaches the drained owner
      // while the DO is handed off waits and is sent on; one that arrives once
      // it was released fails as retryable, and the stub's next call finds the
      // DO's new owner.
      const call = () =>
        fetchJson(
          nodes[1].httpPort,
          '/call-held-stub?name=holder&target=follow-rpc&rpc',
          { timeoutMs: HANG_TIMEOUT_MS }
        );
      let called = await call();
      if (called.status === 500) {
        assert.strictEqual(called.body.retryable, true);
        called = await call();
      }
      assert.strictEqual(called.status, 200, JSON.stringify(called.body));
      assert.strictEqual(called.body.target.nodeId, nodes[1].nodeId);

      await holding;
      assert.deepStrictEqual(await exit, { code: 0, signal: null });
    });
  }
);

test(
  'cluster: a request that reached a draining owner is not replayed',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(2, async ({ nodes }) => {
      const first = await fetchJson(nodes[0].httpPort, '/increment?name=once');
      assert.strictEqual(first.body.nodeId, nodes[0].nodeId);
      const held = await fetchJson(
        nodes[1].httpPort,
        '/hold-stub?name=holder&target=once'
      );
      assert.strictEqual(held.body.nodeId, nodes[1].nodeId);
      assert.strictEqual(held.body.target.nodeId, nodes[0].nodeId);

      // The request waits in the DO on node 0 as node 0 drains. The handoff
      // lets it finish there, and the DO moves with its write; it does not run
      // again on the node that takes the DO over.
      const inFlight = fetchJson(
        nodes[1].httpPort,
        '/call-held-stub?name=holder&target=once&path=/hold-increment%3Fms%3D3000',
        { timeoutMs: 10_000 }
      );
      await sleep(500);
      const exit = await nodes[0].stop({
        signal: 'SIGTERM',
        timeoutMs: 10_000,
      });
      assert.deepStrictEqual(exit, { code: 0, signal: null });
      const response = await inFlight;
      assert.strictEqual(response.status, 200, JSON.stringify(response.body));
      assert.strictEqual(response.body.target.nodeId, nodes[0].nodeId);
      assert.strictEqual(response.body.target.count, 2);

      const state = await fetchJson(nodes[1].httpPort, '/get?name=once', {
        timeoutMs: HANG_TIMEOUT_MS,
      });
      assert.strictEqual(state.status, 200, JSON.stringify(state.body));
      assert.strictEqual(state.body.nodeId, nodes[1].nodeId);
      assert.strictEqual(state.body.count, 2);

      // The held stub lost its connection, not its DO: its next call finds
      // the DO's new owner.
      const next = await fetchJson(
        nodes[1].httpPort,
        '/call-held-stub?name=holder&target=once&path=/increment',
        { timeoutMs: 15_000 }
      );
      assert.strictEqual(next.status, 200, JSON.stringify(next.body));
      assert.strictEqual(next.body.target.nodeId, nodes[1].nodeId);
      assert.strictEqual(next.body.target.count, 3);
    });
  }
);

test(
  'cluster: a held stub follows its DO once the drained owner exited',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(2, async ({ nodes }) => {
      const first = await fetchJson(nodes[0].httpPort, '/increment?name=gone');
      assert.strictEqual(first.body.nodeId, nodes[0].nodeId);
      const held = await fetchJson(
        nodes[1].httpPort,
        '/hold-stub?name=holder&target=gone'
      );
      assert.strictEqual(held.body.nodeId, nodes[1].nodeId);
      assert.strictEqual(held.body.target.nodeId, nodes[0].nodeId);
      const exit = await nodes[0].stop({
        signal: 'SIGTERM',
        timeoutMs: 10_000,
      });
      assert.deepStrictEqual(exit, { code: 0, signal: null });

      // The stub's session to node 0 is gone with it: the call is never sent
      // there, and resolves the DO again.
      const called = await fetchJson(
        nodes[1].httpPort,
        '/call-held-stub?name=holder&target=gone&path=/increment',
        { timeoutMs: 15_000 }
      );
      assert.strictEqual(called.status, 200, JSON.stringify(called.body));
      assert.strictEqual(called.body.target.nodeId, nodes[1].nodeId);
      assert.strictEqual(called.body.target.count, 2);
    });
  }
);

// Load on the DOs `names`: for each, one `/increment` at a time, each sent to
// the next of the nodes `serving()` lists (indices into `nodes`), until
// `finish()`. It resolves to the counts each DO's responses carried, in order,
// and to the requests that failed.
function load(nodes, names, serving) {
  let finished = false;
  const counts = new Map(names.map((name) => [name, []]));
  const failures = [];
  const loops = names.map(async (name, i) => {
    for (let n = i; !finished; n++) {
      const up = serving();
      const node = nodes[up[n % up.length]];
      const response = await fetchJson(
        node.httpPort,
        `/increment?name=${name}`,
        { timeoutMs: 20_000 }
      ).catch((e) => ({ status: 0, body: { error: String(e) } }));
      if (response.status === 200) {
        counts.get(name).push(response.body.count);
      } else {
        failures.push({ name, node: node.nodeId, ...response });
      }
    }
  });
  return async () => {
    finished = true;
    await Promise.all(loops);
    return { counts, failures };
  };
}

// What a node's drain promises the requests of `load`: none fails, none is
// lost (each DO's count goes up by one per response, and the bucket's copy
// ends where the responses did), and none runs twice.
async function assertLoadServed({ counts, failures }, nodes, first = 0) {
  assert.deepStrictEqual(failures, []);
  for (const [name, seen] of counts) {
    assert(seen.length > 0, `no request for ${name} completed`);
    const expected = seen.map((_, i) => first + i + 1);
    assert.deepStrictEqual(seen, expected, `the counts of ${name}`);
    const state = await fetchJson(nodes[0].httpPort, `/get?name=${name}`);
    assert.strictEqual(state.body.count, seen.at(-1), `the count of ${name}`);
  }
}

test(
  'cluster: a node stopping under load hands its DOs to its peers',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(
      3,
      async ({ nodes, bucket, nodeKeys }) => {
        // Node 0 owns every DO the load is on, and the peers send it their
        // requests for them.
        const names = Array.from({ length: 8 }, (_, i) => `loaded-${i}`);
        for (const name of names) {
          const first = await fetchJson(
            nodes[0].httpPort,
            `/increment?name=${name}`
          );
          assert.strictEqual(first.body.nodeId, nodes[0].nodeId);
        }
        await fetchJson(nodes[0].httpPort, '/get?name=slow');
        const finish = load(nodes, names, () => [1, 2]);
        // A request still running on its DO when the handoff's time for
        // requests in flight runs out is cancelled, and not sent again.
        const slow = fetchJson(
          nodes[1].httpPort,
          '/hold-increment?name=slow&ms=15000',
          { timeoutMs: 30_000 }
        );
        await sleep(1000);

        const stoppedAt = Date.now();
        const exit = await nodes[0].stop({
          signal: 'SIGTERM',
          timeoutMs: 30_000,
        });
        assert.deepStrictEqual(exit, { code: 0, signal: null });
        // Within the stop timeout of 6 s, with time for the process to start
        // and exit.
        assert(Date.now() - stoppedAt < 7_500, 'the drain took too long');
        await sleep(1000);

        const result = await finish();
        await assertLoadServed(result, nodes.slice(1), 1);
        const gone = nodeKeys.get(nodes[0].nodeId);
        for (const name of names) {
          const { body } = await fetchJson(
            nodes[1].httpPort,
            `/get?name=${name}`
          );
          assert.notStrictEqual(await bucket.owner(body.id), gone);
        }
        const cut = await slow;
        assert.strictEqual(cut.status, 500, JSON.stringify(cut.body));
        const state = await fetchJson(nodes[1].httpPort, '/get?name=slow');
        assert.strictEqual(state.body.count, 0);
      },
      { configName: 'shortStop' }
    );
  }
);

test(
  'cluster: requests to a DO of a stopping node wait for its handoff and follow it',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(2, async ({ nodes }) => {
      await fetchJson(nodes[0].httpPort, '/increment?name=waited');
      const held = fetchJson(
        nodes[0].httpPort,
        '/hold-increment?name=waited&ms=8000',
        { timeoutMs: 20_000 }
      );
      await sleep(500);
      const exit = nodes[0].stop({ signal: 'SIGTERM', timeoutMs: 20_000 });
      await sleep(300);
      // One from a client of the stopping node, one through its peer: each
      // waits for the request in flight, longer than a retry would, and then
      // runs where the DO went.
      const [local, peer] = await Promise.all(
        [0, 1].map((i) =>
          fetchJson(nodes[i].httpPort, '/increment?name=waited', {
            timeoutMs: 20_000,
          })
        )
      );
      const first = await held;
      assert.strictEqual(first.status, 200, JSON.stringify(first.body));
      assert.deepStrictEqual(
        [first.body.nodeId, first.body.count],
        [nodes[0].nodeId, 2]
      );
      for (const response of [local, peer]) {
        assert.strictEqual(response.status, 200, JSON.stringify(response.body));
        assert.strictEqual(response.body.nodeId, nodes[1].nodeId);
      }
      assert.deepStrictEqual(
        [local.body.count, peer.body.count].sort(),
        [3, 4]
      );
      assert.deepStrictEqual(await exit, { code: 0, signal: null });
    });
  }
);

test(
  "cluster: a stopping node's DOs finish calls to themselves and across nodes",
  SIGTERM_DRAINS,
  async () => {
    await withCluster(2, async ({ nodes }) => {
      for (const [name, i] of [
        ['self', 0],
        ['cycle-a', 0],
        ['cycle-b', 1],
        ['early-a', 0],
        ['early-b', 1],
      ]) {
        const first = await fetchJson(
          nodes[i].httpPort,
          `/increment?name=${name}`
        );
        assert.strictEqual(first.body.nodeId, nodes[i].nodeId);
      }
      // Each call comes back to a DO of node 0 once node 0 drains: one from
      // the DO itself, one from a DO on node 1 that the first called, and one
      // from a DO on node 1 that the first called before node 0 drained.
      const startedAt = Date.now();
      const calls = [
        'self&targets=self&ms=1500',
        'cycle-a&targets=cycle-b,cycle-a&ms=1500',
        'early-a&targets=early-b:1500,early-a',
      ].map((query) =>
        fetchJson(nodes[0].httpPort, `/chain?name=${query}`, {
          timeoutMs: 20_000,
        })
      );
      await sleep(500);
      const exit = nodes[0].stop({ signal: 'SIGTERM', timeoutMs: 20_000 });
      for (const response of await Promise.all(calls)) {
        assert.strictEqual(response.status, 200, JSON.stringify(response.body));
        assert.strictEqual(response.body.nodeId, nodes[0].nodeId);
      }
      // Not held until the requests in flight are cancelled, 10 s in.
      assert(Date.now() - startedAt < 6_000, 'a call waited on the handoff');
      assert.deepStrictEqual(await exit, { code: 0, signal: null });
      for (const [name, count] of [
        ['self', 3],
        ['cycle-a', 3],
        ['cycle-b', 2],
        ['early-a', 3],
        ['early-b', 2],
      ]) {
        const { body } = await fetchJson(
          nodes[1].httpPort,
          `/get?name=${name}`
        );
        assert.strictEqual(body.count, count, name);
      }
    });
  }
);

test(
  "cluster: other DOs' calls to a DO of a stopping node wait for its handoff",
  SIGTERM_DRAINS,
  async () => {
    await withCluster(2, async ({ nodes }) => {
      for (const [name, i] of [
        ['poked', 0],
        ['poker', 1],
      ]) {
        const first = await fetchJson(
          nodes[i].httpPort,
          `/increment?name=${name}`
        );
        assert.strictEqual(first.body.nodeId, nodes[i].nodeId);
      }
      const held = fetchJson(
        nodes[0].httpPort,
        '/hold-increment?name=poked&ms=2000',
        { timeoutMs: 20_000 }
      );
      await sleep(300);
      const exit = nodes[0].stop({ signal: 'SIGTERM', timeoutMs: 20_000 });
      await sleep(200);
      // A DO on node 1 keeps calling the DO, several calls at once, while its
      // handoff waits for the request in flight. No handoff's work made the
      // calls: each waits for the DO and runs where it went, rather than
      // keeping it busy on node 0.
      const poke = await fetchJson(
        nodes[1].httpPort,
        '/poke?name=poker&target=poked&ms=4000&parallel=4',
        { timeoutMs: 20_000 }
      );
      assert.strictEqual(poke.status, 200, JSON.stringify(poke.body));
      const { calls } = poke.body;
      assert(calls.length > 0, 'no call ran');
      for (const call of calls) {
        assert.strictEqual(call.status, 200, JSON.stringify(call));
        assert.strictEqual(call.nodeId, nodes[1].nodeId, JSON.stringify(call));
      }
      const first = await held;
      assert.strictEqual(first.status, 200, JSON.stringify(first.body));
      assert.deepStrictEqual(
        [first.body.nodeId, first.body.count],
        [nodes[0].nodeId, 2]
      );
      assert.deepStrictEqual(await exit, { code: 0, signal: null });
      const { body } = await fetchJson(nodes[1].httpPort, '/get?name=poked');
      assert.strictEqual(body.count, 2 + calls.length);
    });
  }
);

test(
  'cluster: a JS RPC call that waits for a handoff follows the DO',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(2, async ({ nodes }) => {
      await fetchJson(nodes[0].httpPort, '/increment?name=rpc-waited');
      // Node 1 finds the DO on node 0 and keeps the route there.
      const before = await fetchJson(
        nodes[1].httpPort,
        '/rpc-identity?name=rpc-waited'
      );
      assert.strictEqual(before.body.nodeId, nodes[0].nodeId);
      const held = fetchJson(
        nodes[0].httpPort,
        '/hold-increment?name=rpc-waited&ms=1500',
        { timeoutMs: 20_000 }
      );
      await sleep(300);
      const exit = nodes[0].stop({ signal: 'SIGTERM', timeoutMs: 20_000 });
      await sleep(200);
      // The call waits on node 0 for the request in flight, and node 0 sends
      // it on to node 1, which does not route it back on its route from
      // before the handoff.
      const called = await fetchJson(
        nodes[1].httpPort,
        '/rpc-identity?name=rpc-waited',
        { timeoutMs: 20_000 }
      );
      assert.strictEqual(called.status, 200, JSON.stringify(called.body));
      assert.strictEqual(called.body.nodeId, nodes[1].nodeId);
      assert.strictEqual((await held).status, 200);
      assert.deepStrictEqual(await exit, { code: 0, signal: null });
    });
  }
);

test(
  "cluster: a draining node's own JS RPC call that waits for a handoff follows the DO",
  SIGTERM_DRAINS,
  async () => {
    await withCluster(2, async ({ nodes }) => {
      await fetchJson(nodes[0].httpPort, '/increment?name=rpc-own');
      // Node 1 finds the DO on node 0 and keeps the route there.
      const before = await fetchJson(
        nodes[1].httpPort,
        '/rpc-identity?name=rpc-own'
      );
      assert.strictEqual(before.body.nodeId, nodes[0].nodeId);
      const held = fetchJson(
        nodes[0].httpPort,
        '/hold-increment?name=rpc-own&ms=1500',
        { timeoutMs: 20_000 }
      );
      await sleep(300);
      const exit = nodes[0].stop({ signal: 'SIGTERM', timeoutMs: 20_000 });
      await sleep(200);
      // A call made on node 0 itself waits there for the request in flight,
      // then goes to node 1, which does not route it back on its route from
      // before the handoff.
      const called = await fetchJson(
        nodes[0].httpPort,
        '/rpc-identity?name=rpc-own',
        { timeoutMs: 20_000 }
      );
      assert.strictEqual(called.status, 200, JSON.stringify(called.body));
      assert.strictEqual(called.body.nodeId, nodes[1].nodeId);
      assert.strictEqual((await held).status, 200);
      assert.deepStrictEqual(await exit, { code: 0, signal: null });
    });
  }
);

test(
  'cluster: two nodes stopping at once under load hand their DOs to the third',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(3, async ({ nodes, bucket, nodeKeys }) => {
      const names = Array.from({ length: 8 }, (_, i) => `paired-${i}`);
      for (const [i, name] of names.entries()) {
        const first = await fetchJson(
          nodes[i % 2].httpPort,
          `/increment?name=${name}`
        );
        assert.strictEqual(first.body.nodeId, nodes[i % 2].nodeId);
      }
      let serving = [0, 1, 2];
      const finish = load(nodes, names, () => serving);
      await sleep(500);
      const exits = [0, 1].map((i) =>
        nodes[i].stop({ signal: 'SIGTERM', timeoutMs: 20_000 })
      );
      // The stopping nodes keep accepting requests for a while.
      await sleep(1000);
      serving = [2];
      for (const exit of exits) {
        assert.deepStrictEqual(await exit, { code: 0, signal: null });
      }
      await assertLoadServed(await finish(), nodes.slice(2), 1);
      const third = nodeKeys.get(nodes[2].nodeId);
      for (const name of names) {
        const { body } = await fetchJson(
          nodes[2].httpPort,
          `/get?name=${name}`
        );
        assert.strictEqual(await bucket.owner(body.id), third, name);
      }
    });
  }
);

test(
  'cluster: a stopping node hands off a DO it evicted before',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(2, async ({ nodes }) => {
      await fetchJson(nodes[0].httpPort, '/increment?name=evicted');
      // Breaking it evicts it: dormant on node 0, which still owns it.
      await fetchJson(nodes[0].httpPort, '/break?name=evicted');
      await sleep(500);
      const exit = nodes[0].stop({ signal: 'SIGTERM', timeoutMs: 20_000 });
      await sleep(300);
      // Node 0 still accepts requests, and sends this one where the DO goes.
      const response = await fetchJson(
        nodes[0].httpPort,
        '/increment?name=evicted',
        { timeoutMs: 20_000 }
      );
      assert.strictEqual(response.status, 200, JSON.stringify(response.body));
      assert.strictEqual(response.body.nodeId, nodes[1].nodeId);
      assert.strictEqual(response.body.count, 2);
      assert.deepStrictEqual(await exit, { code: 0, signal: null });
    });
  }
);

test(
  'cluster: a DO whose handoff outlasts its deadline moves once its stopping node left',
  SIGTERM_DRAINS,
  async () => {
    await withCluster(
      2,
      async ({ nodes, bucket }) => {
        const first = await fetchJson(
          nodes[0].httpPort,
          '/increment?name=stuck'
        );
        assert.strictEqual(first.body.nodeId, nodes[0].nodeId);
        // The write that releases the DO stalls past the handoff's 4.5 s.
        await bucket.setFaults([
          {
            method: 'PUT',
            prefix: `${objectPrefix(first.body.id)}/own.json`,
            delayMs: 10_000,
            times: 1,
          },
        ]);
        const stoppedAt = Date.now();
        const exit = await nodes[0].stop({
          signal: 'SIGTERM',
          timeoutMs: 20_000,
        });
        assert.deepStrictEqual(exit, { code: 0, signal: null });
        assert(Date.now() - stoppedAt < 7_500, 'the drain took too long');
        assert.deepStrictEqual(await bucket.faultHits(), [1]);
        const after = await fetchJson(
          nodes[1].httpPort,
          '/increment?name=stuck',
          { timeoutMs: 20_000 }
        );
        assert.strictEqual(after.status, 200, JSON.stringify(after.body));
        assert.strictEqual(after.body.nodeId, nodes[1].nodeId);
        assert.strictEqual(after.body.count, 2);
      },
      { configName: 'shortStop' }
    );
  }
);

test('cluster: a request a forwarding peer could not route is retried', async () => {
  await withCluster(2, async ({ nodes, bucket }) => {
    const first = await fetchJson(nodes[1].httpPort, '/increment?name=relay');
    assert.strictEqual(first.body.nodeId, nodes[1].nodeId);
    // Node 0 learns the route to node 1.
    const routed = await fetchJson(nodes[0].httpPort, '/increment?name=relay');
    assert.strictEqual(routed.body.nodeId, nodes[1].nodeId);
    // Node 1 drops its instance, so the next request there reads the DO's
    // owner record, which fails once.
    const broken = await fetchJson(nodes[1].httpPort, '/break?name=relay');
    assert.strictEqual(broken.status, 500, JSON.stringify(broken.body));
    await bucket.setFaults([
      {
        prefix: `${objectPrefix(first.body.id)}/own.json`,
        method: 'GET',
        status: 500,
        times: 1,
      },
    ]);

    // Node 1 fails the request before it reaches the DO, so node 0 sends it
    // again.
    const response = await fetchJson(
      nodes[0].httpPort,
      '/increment?name=relay',
      { timeoutMs: HANG_TIMEOUT_MS }
    );
    assert.deepStrictEqual(await bucket.faultHits(), [1]);
    assert.strictEqual(response.status, 200, JSON.stringify(response.body));
    assert.strictEqual(response.body.nodeId, nodes[1].nodeId);
    assert.strictEqual(response.body.count, 3);
  });
});

test(
  'cluster: a rolling restart under load loses and replays no request',
  SIGTERM_DRAINS,
  async () => {
    const ready = async (port) =>
      (await fetch(`http://127.0.0.1:${port}/ready`).catch(() => null))?.status;
    await withCluster(3, async ({ nodes }) => {
      const names = Array.from({ length: 6 }, (_, i) => `rolled-${i}`);
      let serving = [0, 1, 2];
      const finish = load(nodes, names, () => serving);
      await sleep(500);
      for (const [i, node] of nodes.entries()) {
        const port = node.clusterPort;
        assert.strictEqual(await ready(port), 200);
        const exit = node.stop({ signal: 'SIGTERM', timeoutMs: 20_000 });
        let status = 200;
        while (status === 200) {
          await sleep(20);
          status = await ready(port);
        }
        // Not ready while it drains, then gone.
        assert(status === 503 || status === undefined, `status ${status}`);
        // As a load balancer would, a while after the node is not ready;
        // meanwhile it serves the requests it still gets.
        await sleep(1000);
        serving = serving.filter((n) => n !== i);
        assert.deepStrictEqual(await exit, { code: 0, signal: null });
        await node.start();
        const deadline = Date.now() + 10_000;
        while ((await ready(node.clusterPort)) !== 200) {
          assert(Date.now() < deadline, `${node.nodeId} never got ready`);
          await sleep(50);
        }
        serving = [...serving, i].sort();
        await sleep(500);
      }
      await assertLoadServed(await finish(), nodes);
    });
  }
);

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

// The config of a node that serves the deployments in its bucket, and its disk,
// on which a deployment's Durable Objects are stored (`workerd deploy --help`).
const DEPLOYMENT_NODE = { config: env.NODE_CONFIG, disk: 'do-storage' };

// The encoded message `workerd compile --config-only` writes for the config
// at `path`, constant `constName` if given.
async function compileConfig(path, constName) {
  const args = ['compile', '--config-only', path];
  if (constName !== undefined) args.push(constName);
  const child = spawn(env.WORKERD_BINARY, args, {
    stdio: ['ignore', 'pipe', 'inherit'],
  });
  const chunks = [];
  child.stdout.on('data', (chunk) => chunks.push(chunk));
  const code = await new Promise((resolve, reject) => {
    child.once('error', reject).once('close', resolve);
  });
  assert.strictEqual(code, 0, `compiling ${path} failed`);
  return Buffer.concat(chunks);
}

// Publishes `deployments`, encoded Configs by deployment name, the way any
// tool may, by writing the bucket's layout (src/workerd/server/cluster/deploy.rs):
// each version's objects, then the catalog, created conditionally.
async function deploy(bucket, deployments) {
  const catalog = { format: 1, revision: 1, deployments: {} };
  for (const [name, config] of Object.entries(deployments)) {
    const version = createHash('sha256').update(config).digest('hex');
    const prefix = `deploy/${name}/${version}`;
    await bucket.put(`${prefix}/config.bin`, config);
    const manifest = { format: 1, name, version, requires: [] };
    await bucket.put(`${prefix}/manifest.json`, JSON.stringify(manifest));
    catalog.deployments[name] = { version };
  }
  await bucket.put('deploy/catalog.json', JSON.stringify(catalog), {
    'if-none-match': '*',
  });
}

test('cluster: a fresh cluster serves the deployment in its bucket', async () => {
  const deployment = await compileConfig(env.DEPLOYMENT_CONFIG, 'deployment');
  await withCluster(
    2,
    async ({ nodes, bucket, nodeKeys }) => {
      // The deployment provides "main" and its Durable Object namespace because
      // the node configs define no workers.
      const counts = [];
      let id;
      for (let i = 0; i < 4; i++) {
        const node = nodes[i % nodes.length];
        const { status, body } = await fetchJson(
          node.httpPort,
          '/increment?name=deployed'
        );
        assert.strictEqual(status, 200, JSON.stringify(body));
        counts.push(body.count);
        id = body.id;
      }
      assert.deepStrictEqual(counts, [1, 2, 3, 4]);

      // The bucket stores the clustered object's owner record under the
      // deployment's namespace. The record names one of the nodes.
      const record = await bucket.get(
        `${objectPrefix(id, 'deployed-counter-namespace')}/own.json`
      );
      assert.notStrictEqual(record, null);
      assert([...nodeKeys.values()].includes(JSON.parse(record).node), record);
    },
    {
      ...DEPLOYMENT_NODE,
      before: (bucket) => deploy(bucket, { app: deployment }),
    }
  );
});

test('cluster: a node serves beside deployments it cannot serve', async () => {
  const deployments = {
    app: await compileConfig(env.DEPLOYMENT_CONFIG, 'deployment'),
    // Its service is broken because its worker throws while loading.
    throws: await compileConfig(env.BROKEN_DEPLOYMENTS_CONFIG, 'throws'),
    // It is skipped because it defines "router", as the node does.
    clash: await compileConfig(env.BROKEN_DEPLOYMENTS_CONFIG, 'clash'),
    // It is skipped because it is a node config, with sockets and a cluster.
    extra: await compileConfig(env.NODE_CONFIG),
    // The node skips it because its manifest requires an unsupported feature.
    future: await compileConfig(env.BROKEN_DEPLOYMENTS_CONFIG, 'throws'),
    // The node skips it because its name is not a deployment name.
    '-dash': await compileConfig(env.BROKEN_DEPLOYMENTS_CONFIG, 'throws'),
    // Its service is broken, and so is its Durable Object namespace.
    'broken-do': await compileConfig(env.BROKEN_DEPLOYMENTS_CONFIG, 'brokenDo'),
    // It is skipped because it holds text that is not UTF-8.
    utf8: notUtf8(
      await compileConfig(env.BROKEN_DEPLOYMENTS_CONFIG, 'notUtf8')
    ),
    // It is skipped because its uniqueKey names a directory outside the disk.
    escape: await compileConfig(env.BROKEN_DEPLOYMENTS_CONFIG, 'escape'),
  };
  await withCluster(
    1,
    async ({ nodes: [node], bucket }) => {
      // The node's worker and the accepted deployment keep serving, without
      // the tails and the access binding that cannot run.
      let { status, body } = await fetchJson(node.httpPort, '/own');
      assert.strictEqual(status, 200, JSON.stringify(body));
      ({ status, body } = await fetchJson(
        node.httpPort,
        '/MAIN/increment?name=beside'
      ));
      assert.strictEqual(status, 200, JSON.stringify(body));
      assert.strictEqual(body.count, 1);

      // The broken service, the skipped-service reference, and the reference
      // to a service nothing defines fail requests.
      for (const binding of ['THROWS', 'SKIPPED', 'UNKNOWN']) {
        ({ status, body } = await fetchJson(node.httpPort, `/${binding}/`));
        assert.strictEqual(status, 500, binding);
        assert.match(
          body.error,
          /Service cannot handle requests because its config is invalid/,
          binding
        );
      }

      // A request to the broken namespace's object fails before the node
      // claims the object, so the bucket has no owner record for it.
      ({ status, body } = await fetchJson(node.httpPort, '/BROKEN_DO/x'));
      assert.strictEqual(status, 500, JSON.stringify(body));
      assert.match(
        body.error,
        /Service cannot handle requests because its config is invalid/
      );
      assert.deepStrictEqual(
        await bucket.list(objectPrefix(body.id, 'broken-do-namespace')),
        []
      );

      // The log names each served, skipped, or broken deployment.
      const log = node.stderr;
      assert.match(log, /cluster: serving deployment "app" at version /);
      assert.match(
        log,
        /cluster: skipping a deployment at version \w+: deployment "clash" .*"router"/
      );
      assert.match(
        log,
        /cluster: skipping a deployment at version \w+: deployment "extra" sets `sockets`, `cluster`\. A deployment may contain only `services`/
      );
      assert.match(
        log,
        /cluster: skipping a deployment at version \w+: deploy\/future\/\w+\/manifest\.json requires features this workerd does not support: teleportation/
      );
      assert.match(
        log,
        /cluster: skipping a deployment at version \w+: "-dash" is not a valid deployment name\./
      );
      assert.match(
        log,
        /cluster: skipping a deployment at version \w+: deployment "utf8" has text that is not valid UTF-8 at Config\.services\[0\]\.worker\.modules\[0\]\.esModule\./
      );
      assert.match(
        log,
        /cluster: skipping a deployment at version \w+: service "escapes" in deployment "escape" has Durable Object namespace uniqueKey "\.\.\/\.\.\/cluster-test-escape"\./
      );
      assert.match(
        log,
        /deployment "broken-do": service "broken-do" cannot handle requests because of the errors above\./
      );
      assert.match(
        log,
        /cluster: Worker "router"'s binding "UNKNOWN" refers to a service "nowhere", but no such service is defined\. Neither the instance's config nor a deployment that was read defines "nowhere"\. It may belong to a deployment that could not be read: "-dash", "future"\./
      );
      assert.match(
        log,
        /deployment "app": Worker "router"'s accessBindingService refers to service "main" with a named entrypoint "Nope", but "main" has no such named entrypoint\. "main" is defined by this deployment\./
      );
      assert.match(
        log,
        /deployment "clash": .*"clashing" is defined by this deployment, which was skipped\./
      );
      assert.match(log, /deployment "throws": .*this deployment fails to load/);
      assert.match(
        log,
        /deployment "throws": service "throws" cannot handle requests because of the errors above\./
      );
    },
    {
      config: env.BROKEN_NODE_CONFIG,
      disk: DEPLOYMENT_NODE.disk,
      before: async (bucket) => {
        await deploy(bucket, deployments);
        // Rewrite "future"'s manifest to require a feature.
        const version = createHash('sha256')
          .update(deployments.future)
          .digest('hex');
        const manifest = {
          format: 1,
          name: 'future',
          version,
          requires: ['teleportation'],
        };
        await bucket.put(
          `deploy/future/${version}/manifest.json`,
          JSON.stringify(manifest)
        );
      },
    }
  );
});

// `config` with the first byte of its text "PLACEHOLDER" replaced by one that
// is not valid UTF-8.
function notUtf8(config) {
  const patched = Buffer.from(config);
  const at = patched.indexOf('PLACEHOLDER');
  assert.notStrictEqual(at, -1);
  patched[at] = 0xff;
  return patched;
}

test("cluster: the command line's overrides are for the node's own services", async () => {
  const external = await compileConfig(
    env.BROKEN_DEPLOYMENTS_CONFIG,
    'external'
  );
  await withCluster(
    0,
    async ({ bucket }) => {
      // A deployment defines "ext", but an override names only a service of
      // the node's config, so this one matches nothing.
      const local = await mkdtemp(join(tmpdir(), 'workerd-cluster-'));
      const node = new ClusterNode({
        binary: env.WORKERD_BINARY,
        config: DEPLOYMENT_NODE.config,
        localPath: local,
        disk: DEPLOYMENT_NODE.disk,
        s3Port: bucket.port,
        nodeId: 'overrides',
        extraArgs: ['--external-addr=ext=127.0.0.1:1'],
      });
      await assert.rejects(node.start(), /exited before listening/);
      assert.match(
        node.stderr,
        /Config did not define any external service named "ext" to match the override provided on the command line\./
      );
      await rm(local, { recursive: true });
    },
    { before: (bucket) => deploy(bucket, { external }) }
  );
});

// Runs `workerd deploy --experimental` with `args` against `bucket`, named by
// flags, with the credentials the nodes' config gives in the environment.
// Answers the exit code and what it wrote.
async function workerdDeploy(bucket, args) {
  const child = spawn(
    env.WORKERD_BINARY,
    [
      'deploy',
      '--experimental',
      ...args,
      '--bucket=s3://wdtest/cluster',
      `--endpoint=http://127.0.0.1:${bucket.port}`,
      '--region=us-east-1',
    ],
    {
      stdio: ['ignore', 'pipe', 'pipe'],
      env: {
        ...env,
        AWS_ENDPOINT_URL: undefined,
        AWS_ACCESS_KEY_ID: 'wdtest',
        AWS_SECRET_ACCESS_KEY: 'wdtest-secret',
      },
    }
  );
  let stdout = '';
  let stderr = '';
  child.stdout.on('data', (chunk) => (stdout += chunk));
  child.stderr.on('data', (chunk) => {
    stderr += chunk;
    process.stderr.write(`[deploy] ${chunk}`);
  });
  const code = await new Promise((resolve, reject) => {
    child.once('error', reject).once('close', resolve);
  });
  return { code, stdout, stderr };
}

test("cluster: `workerd deploy` publishes a Cap'n Proto config, and refuses one that conflicts", async () => {
  const args = [env.DEPLOYMENT_CONFIG, 'deployment'];
  await withCluster(
    1,
    async ({ nodes, bucket }) => {
      const { status, body } = await fetchJson(
        nodes[0].httpPort,
        '/increment?name=deployed'
      );
      assert.strictEqual(status, 200, JSON.stringify(body));
      assert.strictEqual(body.count, 1);

      // Publishing the current version again changes nothing.
      const again = await workerdDeploy(bucket, [...args, '--name=app']);
      assert.strictEqual(again.code, 0, again.stderr);
      assert.match(
        again.stdout,
        /catalog revision 1 selects deployment "app" version [0-9a-f]{64}\b/
      );

      // Under another name its service and Durable Object namespace are
      // defined twice: refused before anything is uploaded.
      const other = await workerdDeploy(bucket, [...args, '--name=other']);
      assert.strictEqual(other.code, 1, other.stderr);
      assert.match(
        other.stderr,
        /deployment "other" defines service "main"\. The same service is already defined by deployment "app"/
      );
      assert.strictEqual(
        JSON.parse(await bucket.get('deploy/catalog.json')).revision,
        1
      );
      assert.deepStrictEqual(await bucket.list('deploy/other/'), []);
    },
    {
      ...DEPLOYMENT_NODE,
      // Published before any node runs: the node starts on it.
      before: async (bucket) => {
        const deployed = await workerdDeploy(bucket, [...args, '--name=app']);
        assert.strictEqual(deployed.code, 0, deployed.stderr);
        assert.match(
          deployed.stdout,
          /catalog revision 1 selects deployment "app" version [0-9a-f]{64}\b/
        );
      },
    }
  );
});

test('cluster: `workerd deploy` refuses a deployment that holds more than services', async () => {
  const v1 = await compileConfig(env.DEPLOYMENT_CONFIG, 'deployment');
  await withCluster(
    1,
    async ({ bucket }) => {
      const deployed = await workerdDeploy(bucket, [
        env.NODE_CONFIG,
        '--name=app',
      ]);
      assert.strictEqual(deployed.code, 1, deployed.stderr);
      assert.match(deployed.stderr, /A deployment may contain only `services`/);
      assert.strictEqual(
        JSON.parse(await bucket.get('deploy/catalog.json')).revision,
        1
      );
    },
    {
      ...DEPLOYMENT_NODE,
      before: (bucket) => deploy(bucket, { app: v1 }),
    }
  );
});

// A wrangler project's worker: a Counter Durable Object that `/increment`
// reaches, and `/version`, its VERSION var.
const WRANGLER_WORKER = `
export class Counter {
  constructor(state) {
    this.state = state;
  }
  async fetch() {
    const count = ((await this.state.storage.get('count')) ?? 0) + 1;
    await this.state.storage.put('count', count);
    return Response.json({ count, id: this.state.id.toString() });
  }
}
export default {
  async fetch(request, env) {
    if (new URL(request.url).pathname === '/version') {
      return Response.json({ version: env.VERSION });
    }
    return env.COUNTER.get(env.COUNTER.idFromName('wrangler')).fetch(request);
  },
};
`;

// Writes a wrangler project of WRANGLER_WORKER to `dir`, a no_bundle one with
// the Counter class and `fields`.
async function writeWranglerProject(dir, fields) {
  await writeFile(join(dir, 'index.js'), WRANGLER_WORKER);
  await writeFile(
    join(dir, 'wrangler.jsonc'),
    JSON.stringify({
      main: 'index.js',
      compatibility_date: '2026-01-01',
      no_bundle: true,
      durable_objects: {
        bindings: [{ name: 'COUNTER', class_name: 'Counter' }],
      },
      migrations: [{ tag: 'v1', new_sqlite_classes: ['Counter'] }],
      ...fields,
    })
  );
}

test('cluster: `workerd deploy` publishes a wrangler project', async () => {
  const project = await mkdtemp(join(tmpdir(), 'workerd-wrangler-'));
  // Named "main", the service the nodes' socket serves.
  const configure = (version) =>
    writeWranglerProject(project, { name: 'main', vars: { VERSION: version } });
  try {
    await configure('1');
    await withCluster(
      2,
      async ({ nodes, bucket, nodeKeys }) => {
        // Translated as a deployment: no socket, and no disk of its own.
        assert.deepStrictEqual((await readdir(project)).sort(), [
          'index.js',
          'wrangler.jsonc',
        ]);
        const counts = [];
        let id;
        for (const node of nodes) {
          assert.strictEqual(
            (await fetchJson(node.httpPort, '/version')).body.version,
            '1'
          );
          const { status, body } = await fetchJson(node.httpPort, '/increment');
          assert.strictEqual(status, 200, JSON.stringify(body));
          counts.push(body.count);
          id = body.id;
        }
        assert.deepStrictEqual(counts, [1, 2]);
        // The object is clustered, under the storage key the deployment's name
        // gives its class.
        const record = await bucket.get(
          `${objectPrefix(id, 'main-Counter')}/own.json`
        );
        assert.notStrictEqual(record, null);
        assert(
          [...nodeKeys.values()].includes(JSON.parse(record).node),
          record
        );
      },
      {
        ...DEPLOYMENT_NODE,
        // Published before any node runs: the nodes start on it.
        before: async (bucket) => {
          const deployed = await workerdDeploy(bucket, [project]);
          assert.strictEqual(deployed.code, 0, deployed.stderr);
          assert.match(
            deployed.stdout,
            /catalog revision 1 selects deployment "main" version [0-9a-f]{64}\b/
          );
        },
      }
    );
  } finally {
    await rm(project, { recursive: true, force: true });
  }
});
