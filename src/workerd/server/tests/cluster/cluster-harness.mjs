// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// What the drivers of the cluster tests share: workerd instances over a fake
// S3 (fake-s3.mjs), requests to them, and requests a thread of an instance
// that serves on several (`--threads`) serves.

import { spawn } from 'node:child_process';
import http from 'node:http';
import { mkdir, mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { createHash, randomUUID } from 'node:crypto';
import { join } from 'node:path';
import { env } from 'node:process';
import assert from 'node:assert';
import { setTimeout as sleep } from 'node:timers/promises';
import { FakeS3Server } from './fake-s3.mjs';

const CONTROL_FD = 3;

// A minimal harness specialized for the cluster test. Unlike server-harness.mjs
// it:
//   - allows overriding bindings via process environment (NODE_ID),
//   - points each instance at the fake S3 and at a disk of its own,
//   - supports SIGKILL in addition to SIGTERM for the takeover test.
export class ClusterNode {
  #binary;
  #config;
  #localPath;
  #s3Port;
  #nodeId;
  #child = null;
  #httpPort = null;
  #clusterPort = null;
  #ports = new Map();
  #closed = null;
  #stdoutBuffer = '';
  #stderrBuffer = '';
  #stderrWaiters = new Set();
  #extraArgs;
  #disk;
  #configName;
  #env;
  #compiled;

  // `localPath` is the directory of the config's disk service `disk`, or with
  // a `disk` of null its `--data-dir`, as for a wrangler config. `env`
  // adds to the process's environment, or with `undefined` removes from it. A
  // `configName` of null names no constant, as for a wrangler config.
  constructor({
    binary,
    config,
    configName,
    localPath,
    disk = 'local',
    s3Port,
    nodeId,
    extraArgs = [],
    env = {},
    compiled = false,
  }) {
    this.#extraArgs = extraArgs;
    this.#env = env;
    this.#compiled = compiled;
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

  // The port of the config's socket `name`, once the node said so.
  async port(name) {
    for (let tries = 0; !this.#ports.has(name); tries++) {
      assert(tries < 100, `${this.#nodeId} has no socket ${name}`);
      await sleep(20);
    }
    return this.#ports.get(name);
  }

  // Returns the contents of the node's stdout observed so far: its log under
  // structured logging.
  get stdout() {
    return this.#stdoutBuffer;
  }

  // Returns the contents of the node's stderr observed so far.
  get stderr() {
    return this.#stderrBuffer;
  }

  // Resolves once the node's stderr matches `re`, checked as it arrives.
  waitForStderr(re, ms) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#stderrWaiters.delete(check);
        reject(new Error(`${this.#nodeId}: no ${re} within ${ms} ms`));
      }, ms).unref();
      const check = () => {
        if (!re.test(this.#stderrBuffer)) return;
        clearTimeout(timer);
        this.#stderrWaiters.delete(check);
        resolve();
      };
      this.#stderrWaiters.add(check);
      check();
    });
  }

  async start() {
    assert.strictEqual(this.#child, null);

    // A binary from `workerd compile` takes no command and no config.
    const config = [
      'serve',
      this.#config,
      ...(this.#configName == null ? [] : [this.#configName]),
    ];
    const args = [
      ...(this.#compiled ? [] : config),
      '--experimental',
      '--verbose',
      `--control-fd=${CONTROL_FD}`,
      `--socket-addr=http=127.0.0.1:0`,
    ];
    if (this.#localPath !== undefined) {
      args.push(
        this.#disk === null
          ? `--data-dir=${this.#localPath}`
          : `--directory-path=${this.#disk}=${this.#localPath}`
      );
    }
    args.push(...this.#extraArgs);

    const child = spawn(this.#binary, args, {
      stdio: ['ignore', 'pipe', 'pipe', 'pipe'],
      env: {
        ...env,
        // Drop the runtime on exit, joining its threads, which TSan reports
        // as leaked otherwise.
        KJ_CLEAN_SHUTDOWN: '1',
        NODE_ID: this.#nodeId,
        AWS_ENDPOINT_URL: `http://127.0.0.1:${this.#s3Port}`,
        ...this.#env,
      },
    });
    this.#child = child;

    child.stdout.on('data', (data) => {
      const chunk = data.toString('utf8');
      this.#stdoutBuffer += chunk;
      process.stdout.write(`[${this.#nodeId}] ${chunk}`);
    });

    child.stderr.on('data', (data) => {
      const chunk = data.toString('utf8');
      this.#stderrBuffer += chunk;
      for (const check of this.#stderrWaiters) check();
      // Mirror to our own stderr so test output remains useful.
      process.stderr.write(`[${this.#nodeId}] ${chunk}`);
    });

    // Watch the control fd for the assigned ports. The cluster port's event,
    // if any, comes before the sockets'. The other sockets' ports are kept for
    // `port()`, whether their events come before the http socket's or after.
    const ports = new Map();
    this.#ports = ports;
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
            if (parsed.event === 'listen') {
              ports.set(parsed.socket, parsed.port);
            }
            if (parsed.event === 'listen' && parsed.socket === 'http') {
              resolve(parsed.port);
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

  // How the node's process ends by itself, within `timeoutMs`. A node still
  // running then is killed, and this fails.
  async exited({ timeoutMs }) {
    let timer;
    const late = new Promise((resolve) => {
      timer = setTimeout(resolve, timeoutMs, null);
    });
    const result = await Promise.race([this.#closed, late]);
    clearTimeout(timer);
    if (result === null) {
      await this.stop({ signal: 'SIGKILL', timeoutMs: 2000 });
      assert.fail(`Node ${this.#nodeId} did not exit in ${timeoutMs} ms`);
    }
    this.#child = null;
    this.#httpPort = null;
    this.#clusterPort = null;
    return result;
  }

  // Sends the node `signal` without waiting for it to exit.
  signal(signal) {
    assert(this.#child !== null, 'node not started');
    this.#child.kill(signal);
  }

  // Resolves with the node's exit, `{ code, signal }`, whatever ends it.
  closed() {
    assert(this.#closed !== null, 'node not started');
    return this.#closed;
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
export async function fetchJson(port, path, { timeoutMs } = {}) {
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

// What the `attempts` of a round resolved to, once each has settled. An attempt that failed
// after another was answered is of no interest (the server may have stopped meanwhile); if none
// was answered, the first failure is the round's.
async function settled(attempts) {
  const results = await Promise.allSettled(attempts);
  const answers = results.flatMap((r) =>
    r.status === 'fulfilled' ? [r.value] : []
  );
  const failure = results.find((r) => r.status === 'rejected');
  if (failure !== undefined && answers.every((a) => a.refused)) {
    throw failure.reason;
  }
  return answers;
}

// How many rounds `NodeThread.request` sends before it gives up.
const THREAD_ROUNDS = 8;

// One of the loop threads a node serves on (`--threads`), by the identity of
// its isolate (`thread`). Each request goes to whichever thread takes it, so a
// request names the isolate it is for (`x-thread`), the node's worker refuses
// it on any other thread (409 and `refused`, before doing anything with it),
// and it is sent again until this thread serves it.
//
// The node wakes the sleeping thread with the fewest requests in flight for
// each request, and among equals the one that slept last, so one request at a
// time would keep going to the same warm thread. A round therefore sends
// several attempts a few milliseconds apart, and a worker holds its refusal for
// a while, so that it has a request in flight and the next attempt goes to
// another thread; each round sends twice as many, so that a thread with
// requests of its own in flight is reached too. The attempts share an
// `x-attempt`, so this thread acts on one of them only.
export class NodeThread {
  #port;
  #threads;

  // `threads` is how many threads the node serves on.
  constructor(port, thread, threads) {
    this.#port = port;
    this.thread = thread;
    this.#threads = threads;
  }

  // As `fetchJson`, served by this thread.
  async fetchJson(path, { timeoutMs = 30_000 } = {}) {
    const { status, body } = await this.request(path, { timeoutMs });
    try {
      return { status, body: JSON.parse(body.toString('utf8')) };
    } catch {
      throw new Error(`Non-JSON response to ${path}: ${body}`);
    }
  }

  // A request for `path` with `method` and `body` (a Buffer, if any), served
  // by this thread: its `{ status, headers, body }`, the body a Buffer.
  async request(path, { timeoutMs = 30_000, method = 'GET', body } = {}) {
    const attempt = randomUUID();
    for (let round = 1; round <= THREAD_ROUNDS; round++) {
      const attempts = [];
      for (let i = 0; i < this.#threads * 2 ** (round - 1); i++) {
        const sent = this.#send(path, timeoutMs, attempt, method, body);
        // A failure is the round's only if no attempt was answered (`settled`).
        sent.catch(() => {});
        attempts.push(sent);
        await sleep(2);
      }
      const answers = (await settled(attempts)).filter((a) => !a.refused);
      if (answers.length > 0) return answers[0];
    }
    assert.fail(
      `thread ${this.thread} served none of ${THREAD_ROUNDS} ${path}`
    );
  }

  // One attempt: its `{ status, headers, body, refused }`.
  #send(path, timeoutMs, attempt, method, body) {
    return new Promise((resolve, reject) => {
      const req = http.request(
        {
          agent: false,
          host: '127.0.0.1',
          port: this.#port,
          path,
          method,
          headers: { 'x-thread': this.thread, 'x-attempt': attempt },
        },
        (res) => {
          const chunks = [];
          res.on('data', (chunk) => chunks.push(chunk));
          res.on('error', reject);
          res.on('end', () => {
            const answer = Buffer.concat(chunks);
            let refused = false;
            if (res.statusCode === 409) {
              try {
                refused = JSON.parse(answer.toString('utf8')).refused === true;
              } catch {
                // Not a refusal.
              }
            }
            resolve({
              status: res.statusCode,
              headers: res.headers,
              body: answer,
              refused,
            });
          });
        }
      );
      req.setTimeout(timeoutMs, () => {
        req.destroy(
          new Error(`${path} did not complete within ${timeoutMs}ms`)
        );
      });
      req.on('error', reject);
      req.end(body);
    });
  }
}

// Each of the loop threads `node` serves on. The node's worker answers
// `/thread` with its isolate's identity, which every thread has its own of. A
// round sends a request per thread at once, each answered after a while: a
// thread with one in flight is passed over for one with none, so a round
// usually finds every thread.
export async function nodeThreads(node, threads) {
  const found = new Set();
  for (let round = 0; round < THREAD_ROUNDS && found.size < threads; round++) {
    const answers = [];
    for (let i = 0; i < threads; i++) {
      answers.push(fetchJson(node.httpPort, '/thread?ms=250'));
      await sleep(2);
    }
    for (const { status, body } of await Promise.all(answers)) {
      assert.strictEqual(status, 200, JSON.stringify(body));
      found.add(body.thread);
    }
  }
  assert.strictEqual(
    found.size,
    threads,
    `${node.nodeId} served requests on ${found.size} threads`
  );
  return [...found].map(
    (thread) => new NodeThread(node.httpPort, thread, threads)
  );
}

// The fake S3 the cluster's state lives in (fake-s3.mjs), or in its "gcs"
// dialect the fake Cloud Storage, which the test's own requests reach without
// a token. The tests share one of each, emptied for each.
const fakeServers = new Map();

export class FakeS3 {
  #server;
  #headers;

  // An empty bucket of `dialect`, "s3" or "gcs".
  static async fresh(dialect = 's3') {
    let server = fakeServers.get(dialect);
    if (server === undefined) {
      server = new FakeS3Server({ dialect });
      await server.listen();
      fakeServers.set(dialect, server);
    }
    server.reset();
    return new FakeS3(server, dialect === 'gcs' ? { 'x-fake-admin': '1' } : {});
  }

  constructor(server, headers) {
    this.#server = server;
    this.#headers = headers;
  }

  // Where a node's bucket client and token requests go, for a fake Cloud
  // Storage.
  get gcsEnv() {
    const host = `127.0.0.1:${this.port}`;
    return {
      STORAGE_EMULATOR_HOST: host,
      GCE_METADATA_HOST: host,
      GOOGLE_APPLICATION_CREDENTIALS: undefined,
    };
  }

  get port() {
    return this.#server.port;
  }

  // The keys under `prefix` in the cluster's bucket, whose layout is under
  // `cluster/v1/`.
  async list(prefix) {
    const query = new URLSearchParams({
      'list-type': '2',
      prefix: `cluster/v1/${prefix}`,
    });
    const res = await fetch(`http://127.0.0.1:${this.port}/wdtest/?${query}`, {
      headers: this.#headers,
    });
    const xml = await res.text();
    return [...xml.matchAll(/<Key>([^<]*)<\/Key>/g)].map((m) =>
      m[1].slice('cluster/v1/'.length)
    );
  }

  async get(key) {
    const res = await fetch(
      `http://127.0.0.1:${this.port}/wdtest/cluster/v1/${key}`,
      { headers: this.#headers }
    );
    return res.status === 404 ? null : await res.text();
  }

  async delete(key) {
    await fetch(`http://127.0.0.1:${this.port}/wdtest/cluster/v1/${key}`, {
      method: 'DELETE',
      headers: this.#headers,
    });
  }

  async put(key, body, headers = {}) {
    const res = await fetch(
      `http://127.0.0.1:${this.port}/wdtest/cluster/v1/${key}`,
      { method: 'PUT', body, headers: { ...this.#headers, ...headers } }
    );
    assert.strictEqual(res.status, 200, `PUT ${key} answered ${res.status}`);
  }

  // How many writes to `key` failed their precondition.
  async rejected(key) {
    const query = new URLSearchParams({ key: `cluster/v1/${key}` });
    return (
      await fetch(`http://127.0.0.1:${this.port}/_rejected?${query}`)
    ).json();
  }

  // A fake Cloud Storage's tokens: with `account`, sets their lifetime
  // (`expiresIn`) or the service account (`email`, `jwk`); answers how many
  // it issued and refused (fake-s3.mjs).
  async gcs(account) {
    const res = await fetch(`http://127.0.0.1:${this.port}/_gcs`, {
      method: account === undefined ? 'GET' : 'POST',
      body: account === undefined ? undefined : JSON.stringify(account),
    });
    return res.json();
  }

  // The key of the node that owns the Counter with this ID, of the namespace
  // whose uniqueKey is `uniqueKey` if given, or null when no owner record
  // exists.
  async owner(id, uniqueKey) {
    const record = await this.get(`${objectPrefix(id, uniqueKey)}/own.json`);
    return record === null ? null : JSON.parse(record).node;
  }

  // Arms `faults` (fake-s3.mjs), each `prefix` a key in the cluster's layout,
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

  // How many GET, PUT and DELETE requests the bucket served for `key`, a key
  // in the cluster's layout. The fake counts a key without its first
  // segment, the bucket's `cluster/`.
  async requests(key) {
    const query = new URLSearchParams({ key: `v1/${key}` });
    return (
      await fetch(`http://127.0.0.1:${this.port}/_requests?${query}`)
    ).json();
  }

  // How many body bytes GETs of `key`, a key in the cluster's layout, were
  // served.
  async served(key) {
    const query = new URLSearchParams({ key: `v1/${key}` });
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
// service `disk` (with null, the --data-dir) is a directory of each node's
// own, once `before` has prepared the bucket, of `dialect` ("s3" or "gcs",
// FakeS3.fresh). Node `i` gets the extra arguments `extraArgs(i, root)` and
// the extra environment variables `nodeEnv(i, root, bucket)`, where `root` is
// the test's temporary directory, and runs the binary that `nodeBinary(i,
// root)` names (`compiled` when `workerd compile` built it).
export async function withCluster(
  numNodes,
  fn,
  {
    extraArgs = () => [],
    nodeEnv = async () => ({}),
    nodeBinary = async () => ({ binary: env.WORKERD_BINARY, compiled: false }),
    config = env.WD_TEST_CONFIG,
    configName = 'config',
    disk = 'local',
    before = async () => {},
    dialect = 's3',
  } = {}
) {
  // Each test run gets its own bucket and disks, so state doesn't leak
  // between tests.
  const root = await mkdtemp(join(tmpdir(), 'workerd-cluster-'));
  const bucket = await FakeS3.fresh(dialect);
  const nodes = [];
  const nodeKeys = new Map();
  try {
    await before(bucket);
    for (let i = 0; i < numNodes; i++) {
      const localPath = join(root, `node${i}`);
      await mkdir(localPath);
      nodes.push(
        new ClusterNode({
          ...(await nodeBinary(i, root)),
          config,
          configName,
          localPath,
          disk,
          s3Port: bucket.port,
          nodeId: `node${i}`,
          extraArgs: await extraArgs(i, root),
          env: await nodeEnv(i, root, bucket),
        })
      );
    }
    await Promise.all(nodes.map((node) => node.start()));

    // A node writes its lease record, with the address peers reach it at, as it
    // joins the cluster, before it listens.
    const deadline = Date.now() + 10_000;
    let keys = [];
    while (Date.now() < deadline) {
      keys = await bucket.nodes();
      if (keys.length >= numNodes) break;
      await sleep(50);
    }
    assert.strictEqual(
      keys.length,
      numNodes,
      `expected one lease record per node, got: [${keys.join(', ')}]`
    );
    for (const key of keys) {
      const { address } = JSON.parse(await bucket.get(`nodes/${key}.json`));
      const node = nodes.find((n) => address.endsWith(`:${n.clusterPort}`));
      assert(node !== undefined, `no node advertises ${address}`);
      assert(!nodeKeys.has(node.nodeId), `${node.nodeId} has two records`);
      nodeKeys.set(node.nodeId, key);
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
    await rm(root, { recursive: true, force: true });
  }
}

// Where a Durable Object's records are in the bucket: a hash of its
// namespace's uniqueKey, then its ID.
export function objectPrefix(id, uniqueKey = 'counter-test-namespace') {
  const hash = createHash('sha256');
  for (const part of ['workerd cluster namespace v1', '', uniqueKey]) {
    const length = Buffer.alloc(8);
    length.writeBigUInt64BE(BigInt(Buffer.byteLength(part)));
    hash.update(length).update(part);
  }
  return `do/${hash.digest().subarray(0, 16).toString('base64url')}/${id}`;
}

// Polls `check` until it answers true, for at most `ms`.
export async function until(check, ms, message) {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    if (await check()) return;
    await sleep(200);
  }
  assert.fail(message);
}
