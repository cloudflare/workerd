// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Integration test for the cluster mode described in scalable-durable-objects.md.
//
// The test spawns several workerd instances pointing at the same shared
// directory, then exercises:
//   1. Consistent DO state across instances (single-writer correctness).
//   2. Transparent forwarding when a request arrives at the "wrong" instance.
//   3. Takeover after a node is killed.
//   4. Alarms are rejected with a clear error in cluster mode.
//   5. No `metadata.sqlite` is ever created (no AlarmScheduler in cluster mode).
//   6. Storing/loading a DO stub without calling it takes no ownership lock.
//   7. A broken DO releases its ownership lock even while stubs still pin it,
//      so it can be re-instantiated (on any node) instead of hanging requests.
//   8. Persistent stubs to restored targets (facets, RpcTargets, and chains of
//      them) rooted at a DO on one node can be stored and redeemed from a DO on
//      another node; the restore chain runs on the root DO's owner. This holds
//      whether the stub arrives as a token or was minted in-process and passed
//      through props.
//   9. A stub that has resolved to a remote DO keeps failing once that DO
//      breaks, while a fresh stub reaches the re-instantiated DO.
//
// Two variants are run: the unix-socket cluster network mode (default) and a
// localhost CIDR (`127.0.0.0/8`) IP-socket mode that exercises the registry
// file locking and TCP peer connections separately. The variant is selected
// by the WD_TEST_CONFIG environment variable supplied by the BUILD target.

import { spawn } from 'node:child_process';
import { mkdtemp, readdir, readFile, rm, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
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

const CONTROL_FD = 3;

// A minimal harness specialized for the cluster test. Unlike server-harness.mjs
// it:
//   - allows overriding bindings via process environment (NODE_ID),
//   - permits multiple instances to share a directory,
//   - supports SIGKILL in addition to SIGTERM for the takeover test.
class ClusterNode {
  #binary;
  #config;
  #sharedPath;
  #nodeId;
  #child = null;
  #httpPort = null;
  #closed = null;
  #stderrBuffer = '';

  constructor({ binary, config, sharedPath, nodeId }) {
    this.#binary = binary;
    this.#config = config;
    this.#sharedPath = sharedPath;
    this.#nodeId = nodeId;
  }

  get nodeId() {
    return this.#nodeId;
  }

  get httpPort() {
    assert(this.#httpPort !== null, 'node not started');
    return this.#httpPort;
  }

  // Returns the contents of the node's stderr observed so far. Useful for
  // verifying that internal error messages (e.g. the cluster alarm rejection)
  // were emitted.
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
      `--directory-path=shared=${this.#sharedPath}`,
      `--socket-addr=http=127.0.0.1:0`,
    ];

    const child = spawn(this.#binary, args, {
      stdio: ['ignore', 'inherit', 'pipe', 'pipe'],
      env: { ...env, NODE_ID: this.#nodeId },
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

// Returns the hex-encoded public key of the node that currently owns the lock
// file at `path` (the node's registry entry name), or null if the file is
// absent or empty.
async function readLockOwner(path) {
  let content;
  try {
    content = await readFile(path);
  } catch (err) {
    if (err.code === 'ENOENT') return null;
    throw err;
  }
  return content.length === 0 ? null : content.toString('hex');
}

// Starts `numNodes` workerd instances sharing one directory and runs `fn` with:
//   nodes      the ClusterNode instances, in start order
//   sharedPath the shared directory
//   nodeKeys   Map from nodeId to the node's registry key (64-char hex), which
//              is also what ownership lock files contain
async function withCluster(numNodes, fn) {
  // Each test run gets its own shared directory, so the registry/lock-file
  // state doesn't leak between tests.
  const sharedPath = await mkdtemp(join(tmpdir(), 'workerd-cluster-'));
  const registryDir = join(sharedPath, 'workerd-registry');
  const nodes = [];
  const nodeKeys = new Map();
  try {
    for (let i = 0; i < numNodes; i++) {
      const node = new ClusterNode({
        binary: env.WORKERD_BINARY,
        config: env.WD_TEST_CONFIG,
        sharedPath,
        nodeId: `node${i}`,
      });
      await node.start();
      nodes.push(node);

      // Each node writes its registry entry before it reports its listening
      // socket, but allow a small grace period for filesystem visibility. Since
      // nodes start one at a time, the entry that is new is this node's.
      const known = new Set(nodeKeys.values());
      const deadline = Date.now() + 5000;
      let fresh = [];
      while (Date.now() < deadline) {
        let entries = [];
        try {
          entries = await readdir(registryDir);
        } catch (_) {
          // Not created yet.
        }
        fresh = entries.filter((name) => !known.has(name));
        if (fresh.length > 0) break;
        await sleep(50);
      }
      assert.strictEqual(
        fresh.length,
        1,
        `expected exactly one new registry entry after starting ${node.nodeId}, got: [${fresh.join(', ')}]`
      );
      nodeKeys.set(node.nodeId, fresh[0]);
    }
    await fn({ nodes, sharedPath, nodeKeys });
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
    await rm(sharedPath, { recursive: true, force: true });
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

    // Hard-kill the owner. SIGKILL leaves the registry file in place, so the
    // survivor has to detect death via a failed RPC + dead-peer cleanup probe.
    await nodes[ownerIndex].stop({ signal: 'SIGKILL', timeoutMs: 2000 });

    // Hit the survivor repeatedly. The first request may fail or take time
    // while the survivor's RPC attempt to the dead owner is in flight; once
    // the dead-peer cleanup completes, subsequent requests succeed and the
    // survivor takes ownership. The persisted count must be preserved.
    let response = null;
    let lastErr = null;
    const deadline = Date.now() + 30_000;
    while (Date.now() < deadline) {
      try {
        response = await fetchJson(
          survivor.httpPort,
          '/increment?name=takeover'
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

test('cluster: alarms are rejected with a clear error', async () => {
  await withCluster(1, async ({ nodes, sharedPath }) => {
    // Fire the request. In cluster mode `setAlarm()` does not throw
    // synchronously to the JS code -- the rejection happens during output-gate
    // flush, after the worker's fetch handler has already returned. The
    // observable behaviour is therefore a non-2xx HTTP status, while the
    // canonical error message is logged to stderr where the test can detect
    // it. (The spec mandates the message "Durable Object alarms are not yet
    // supported in cluster mode".)
    let response;
    try {
      response = await fetch(
        `http://127.0.0.1:${nodes[0].httpPort}/set-alarm?name=alarm-test`
      );
    } catch (err) {
      // A network-level failure is acceptable too: it confirms that the alarm
      // path did not silently succeed.
      response = null;
    }
    if (response !== null) {
      // Drain the body to allow logging to flush.
      try {
        await response.text();
      } catch (_) {
        // ignore
      }
      assert.notStrictEqual(
        Math.floor(response.status / 100),
        2,
        `expected non-2xx for set-alarm in cluster mode, got ${response.status}`
      );
    }

    // Wait briefly for stderr to flush the error message.
    const deadline = Date.now() + 5000;
    const expectedMessage =
      'Durable Object alarms are not yet supported in cluster mode';
    while (
      Date.now() < deadline &&
      !nodes[0].stderr.includes(expectedMessage)
    ) {
      await sleep(50);
    }
    assert(
      nodes[0].stderr.includes(expectedMessage),
      `expected stderr to contain the cluster alarm rejection message ` +
        `("${expectedMessage}"); stderr so far:\n${nodes[0].stderr}`
    );

    // No per-namespace metadata.sqlite should exist anywhere under the
    // shared directory -- in cluster mode the AlarmScheduler is never
    // constructed.
    const nsDir = join(sharedPath, 'counter-test-namespace');
    let entries = [];
    try {
      entries = await readdir(nsDir);
    } catch (err) {
      // Namespace dir might not exist if the DO was never created on disk
      // because the request failed before storage was opened. That is a
      // valid (and even stronger) signal that no metadata.sqlite was made.
    }
    assert(
      !entries.some((name) => name.startsWith('metadata.sqlite')),
      `cluster mode must not create metadata.sqlite; found: ${entries.join(', ')}`
    );
  });
});

test('cluster: registry directory is populated for each running instance', async () => {
  await withCluster(2, async ({ nodes, sharedPath }) => {
    // Give the instances a brief moment to settle. Both should have written
    // their registry entries before they reported their listening sockets,
    // but allow a small grace period for filesystem visibility.
    const registryDir = join(sharedPath, 'workerd-registry');
    let entries = [];
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
      try {
        entries = await readdir(registryDir);
      } catch (_) {
        entries = [];
      }
      if (entries.length >= nodes.length) break;
      await sleep(100);
    }
    assert.strictEqual(
      entries.length,
      nodes.length,
      `expected ${nodes.length} registry entries, got ${entries.length}: [${entries.join(', ')}]`
    );
    // Each entry should be a 64-char hex public key.
    for (const name of entries) {
      assert.match(
        name,
        /^[0-9a-f]{64}$/,
        `registry entry name should be 64-char hex, got: ${name}`
      );
    }
  });
});

async function exists(path) {
  try {
    await stat(path);
    return true;
  } catch (err) {
    if (err.code === 'ENOENT') return false;
    throw err;
  }
}

test('cluster: storing and loading a stub does not take the ownership lock', async () => {
  await withCluster(2, async ({ nodes, sharedPath }) => {
    const nsDir = join(sharedPath, 'counter-test-namespace');
    const locksDir = join(nsDir, 'locks');

    // DO "holder" (on whichever node claims it) stores a stub to DO "lazy" and
    // reads it back. Serializing and deserializing the stub must not claim
    // "lazy": no lock file, and no storage for it.
    const stored = await fetchJson(
      nodes[1].httpPort,
      '/store-stub?name=holder&target=lazy'
    );
    assert.strictEqual(stored.status, 200, JSON.stringify(stored.body));
    assert.strictEqual(stored.body.hasStub, true);
    const holderId = stored.body.id;
    const targetId = stored.body.targetId;
    assert.notStrictEqual(holderId, targetId);
    assert(
      await exists(join(locksDir, holderId)),
      `the holder DO itself must be locked`
    );
    const lockFile = join(locksDir, targetId);
    assert(
      !(await exists(lockFile)),
      `storing/loading a stub must not create the lock file ${lockFile}`
    );
    const nsEntries = await readdir(nsDir);
    assert(
      !nsEntries.some((name) => name.startsWith(targetId)),
      `storing/loading a stub must not create actor storage; found: ${nsEntries.join(', ')}`
    );

    // Calling the loaded stub claims ownership and creates the lock file.
    const called = await fetchJson(
      nodes[1].httpPort,
      '/call-stored-stub?name=holder'
    );
    assert.strictEqual(called.status, 200, JSON.stringify(called.body));
    assert.strictEqual(called.body.target.id, targetId);
    assert.strictEqual(called.body.target.count, 0);
    assert(
      await exists(lockFile),
      `calling the stub must create the lock file ${lockFile}`
    );
  });
});

// Requests that would hang forever if a broken actor kept its ownership lock
// are bounded by this timeout so the failure is a clear error.
const HANG_TIMEOUT_MS = 5000;

function lockFilePath(sharedPath, id) {
  return join(sharedPath, 'counter-test-namespace', 'locks', id);
}

test('cluster: a broken DO pinned by a stub does not keep its ownership lock', async () => {
  await withCluster(2, async ({ nodes, sharedPath, nodeKeys }) => {
    // Node 0 claims "pinned". DO "holder", served by node 1, then obtains a
    // stub to it, uses it, and keeps it for as long as "holder" lives. Through
    // node 0's bootstrap for that stub, node 0's container for "pinned" stays
    // referenced for the rest of the test.
    const first = await fetchJson(nodes[0].httpPort, '/increment?name=pinned');
    assert.strictEqual(first.status, 200, JSON.stringify(first.body));
    assert.strictEqual(first.body.count, 1);
    const owner = first.body.nodeId;
    const lockFile = lockFilePath(sharedPath, first.body.id);
    assert.strictEqual(await readLockOwner(lockFile), nodeKeys.get(owner));

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
      await readLockOwner(lockFile),
      nodeKeys.get(after.body.nodeId),
      'the lock file must name the node that re-instantiated the DO'
    );
  });
});

test('cluster: a DO whose constructor fails releases its ownership lock', async () => {
  await withCluster(2, async ({ nodes, sharedPath, nodeKeys }) => {
    const first = await fetchJson(nodes[0].httpPort, '/increment?name=ctor');
    assert.strictEqual(first.status, 200, JSON.stringify(first.body));
    const lockFile = lockFilePath(sharedPath, first.body.id);

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
      await readLockOwner(lockFile),
      nodeKeys.get(after.body.nodeId),
      'the lock file must name the node that re-instantiated the DO'
    );
  });
});

test('cluster: aborting a DO with a request in flight lets it be re-instantiated immediately', async () => {
  await withCluster(2, async ({ nodes, sharedPath, nodeKeys }) => {
    const first = await fetchJson(
      nodes[0].httpPort,
      '/increment?name=inflight'
    );
    assert.strictEqual(first.status, 200, JSON.stringify(first.body));
    const owner = first.body.nodeId;
    const lockFile = lockFilePath(sharedPath, first.body.id);

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
      await readLockOwner(lockFile),
      nodeKeys.get(after.body.nodeId),
      'the lock file must name the node that re-instantiated the DO'
    );

    const held = await holding;
    assert.strictEqual(held.status, 500, JSON.stringify(held.body));
  });
});

// Sets up the cross-node persistent-stub scenario shared by the tests below:
// DO "root" is claimed by node 0, then DO "holder", served through node 1 (and
// therefore claimed by node 1), asks "root" to vend a persistent stub of the
// given kind and stores it. `liveRestores` is how many times vending itself is
// expected to run root's [restore](). Returns the two DOs' node IDs and root's
// id.
async function vendAcrossNodes(nodes, kind, liveRestores = 1) {
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
  const holderOwner = vended.body.nodeId;
  assert.notStrictEqual(
    holderOwner,
    rootOwner,
    'the test requires root and holder to live on different nodes'
  );

  // Vending ran [restore]() live. Storing and loading the stub must not have
  // replayed it.
  const state = await fetchJson(nodes[1].httpPort, '/get?name=root');
  assert.strictEqual(state.status, 200, JSON.stringify(state.body));
  assert.strictEqual(state.body.restores, liveRestores);

  return { rootOwner, holderOwner, rootId: first.body.id };
}

test('cluster: a restored facet stub is redeemed on the root DO owner', async () => {
  await withCluster(2, async ({ nodes, sharedPath, nodeKeys }) => {
    const { rootOwner, rootId } = await vendAcrossNodes(nodes, 'facet');

    // Redeeming from holder (node 1) forwards the whole token to root's owner,
    // which replays [restore]() and runs the facet there.
    const used = await fetchJson(
      nodes[1].httpPort,
      '/use-stub?name=holder&kind=facet',
      { timeoutMs: HANG_TIMEOUT_MS }
    );
    assert.strictEqual(used.status, 200, JSON.stringify(used.body));
    assert.strictEqual(used.body.result.count, 1);
    assert.strictEqual(used.body.result.nodeId, rootOwner);
    let state = await fetchJson(nodes[1].httpPort, '/get?name=root');
    assert.strictEqual(state.body.restores, 2);

    // Break root so the next redemption has to re-instantiate it (on whichever
    // node claims it) and replay [restore]() from scratch. The facet's storage
    // lives under root's, so its count carries over.
    const broken = await fetchJson(nodes[1].httpPort, '/break?name=root');
    assert.strictEqual(broken.status, 500, JSON.stringify(broken.body));

    const again = await fetchJson(
      nodes[1].httpPort,
      '/use-stub?name=holder&kind=facet',
      { timeoutMs: HANG_TIMEOUT_MS }
    );
    assert.strictEqual(again.status, 200, JSON.stringify(again.body));
    assert.strictEqual(again.body.result.count, 2);
    assert.strictEqual(
      await readLockOwner(lockFilePath(sharedPath, rootId)),
      nodeKeys.get(again.body.result.nodeId),
      'the facet must run on the node that now owns root'
    );
    state = await fetchJson(nodes[1].httpPort, '/get?name=root');
    assert.strictEqual(state.body.restores, 3);
  });
});

test('cluster: a restored RpcTarget stub is redeemed on the root DO owner', async () => {
  await withCluster(2, async ({ nodes }) => {
    const { rootOwner } = await vendAcrossNodes(nodes, 'rpc');

    // The RpcTarget mutates root's own storage, so it must run in root's
    // context on root's owner.
    const used = await fetchJson(
      nodes[1].httpPort,
      '/use-stub?name=holder&kind=rpc&amount=10',
      { timeoutMs: HANG_TIMEOUT_MS }
    );
    assert.strictEqual(used.status, 200, JSON.stringify(used.body));
    assert.deepStrictEqual(used.body.result, { count: 11, nodeId: rootOwner });

    const state = await fetchJson(nodes[0].httpPort, '/get?name=root');
    assert.strictEqual(state.body.count, 11);
    assert.strictEqual(state.body.restores, 2);
  });
});

test('cluster: a two-level restore chain is redeemed on the root DO owner', async () => {
  await withCluster(2, async ({ nodes }) => {
    // root -> facet -> sub-facet. The facet's own [restore]() must run with a
    // working ctx.restore(), which only holds when the entire chain is replayed
    // in-process on root's owner. Vending runs root's [restore]() twice: once
    // live for the facet stub, and once more when holder calls vendSub() on
    // that stub, since a stub received over RPC is replayed on first use.
    const { rootOwner } = await vendAcrossNodes(nodes, 'chained', 2);

    const used = await fetchJson(
      nodes[1].httpPort,
      '/use-stub?name=holder&kind=chained',
      { timeoutMs: HANG_TIMEOUT_MS }
    );
    assert.strictEqual(used.status, 200, JSON.stringify(used.body));
    assert.strictEqual(used.body.result.count, 1);
    assert.strictEqual(used.body.result.nodeId, rootOwner);

    const state = await fetchJson(nodes[0].httpPort, '/get?name=root');
    assert.strictEqual(state.body.restores, 3);
  });
});

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

test('cluster: a freshly minted RpcTarget stub passed via props follows its DO to a new owner', async () => {
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

    // Move root to the other node: break it, then re-instantiate it from there.
    const broken = await fetchJson(nodes[1].httpPort, '/break?name=root');
    assert.strictEqual(broken.status, 500, JSON.stringify(broken.body));
    const moved = await fetchJson(nodes[1].httpPort, '/increment?name=root', {
      timeoutMs: HANG_TIMEOUT_MS,
    });
    assert.strictEqual(moved.status, 200, JSON.stringify(moved.body));
    assert.strictEqual(moved.body.count, 2);
    assert.notStrictEqual(moved.body.nodeId, originalOwner);

    // Now let the Consumer use its stub. The restore must be forwarded to the
    // new owner as a whole token, not replayed over a remote vendor channel.
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
