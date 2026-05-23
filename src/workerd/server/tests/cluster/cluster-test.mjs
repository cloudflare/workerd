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
//
// Two variants are run: the unix-socket cluster network mode (default) and a
// localhost CIDR (`127.0.0.0/8`) IP-socket mode that exercises the registry
// file locking and TCP peer connections separately. The variant is selected
// by the WD_TEST_CONFIG environment variable supplied by the BUILD target.

import { spawn } from 'node:child_process';
import { mkdtemp, readdir, rm } from 'node:fs/promises';
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

async function fetchJson(port, path, init) {
  const url = `http://127.0.0.1:${port}${path}`;
  const res = await fetch(url, init);
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

async function withCluster(numNodes, fn) {
  // Each test run gets its own shared directory, so the registry/lock-file
  // state doesn't leak between tests.
  const sharedPath = await mkdtemp(join(tmpdir(), 'workerd-cluster-'));
  const nodes = [];
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
    }
    await fn({ nodes, sharedPath });
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
