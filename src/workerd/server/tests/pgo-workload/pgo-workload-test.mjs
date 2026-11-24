// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// The serving workload that trains the release build's PGO profile (build/pgo/README.md):
// `workerd serve` of a few Workers, each sent many requests by concurrent clients. The tests only
// do what `workerd test` does, starting a Worker and handing it a few events, so without this
// workload the profile wouldn't see the HTTP server, or a Worker handling one request after
// another.
//
// It checks every response, so that a broken Worker fails the test rather than training the
// profile on error paths. Normally it sends a few requests per path; under the PGO training
// wrapper (build/pgo/profile-env.sh), which sets WORKERD_PGO_TRAINING, it sends many.

import { execFileSync } from 'node:child_process';
import {
  copyFileSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { env } from 'node:process';
import { test } from 'node:test';
import assert from 'node:assert';
import { WorkerdServerHarness } from '../server-harness.mjs';

const SOCKETS = ['hello', 'hello-async', 'mixed'];
const TRAINING = env.WORKERD_PGO_TRAINING === '1';
const CONCURRENCY = 16;

// The newest compatibility date the binary supports: `workerd --version` prints
// "workerd <date>".
function newestCompatDate() {
  return execFileSync(env.WORKERD_BINARY, ['--version'], { encoding: 'utf8' })
    .trim()
    .split(' ')[1];
}

// The config, with the newest compatibility date in place of COMPAT_DATE, in a directory of its
// own with the Workers it embeds.
function writeConfig() {
  const dir = mkdtempSync(join(env.TEST_TMPDIR ?? tmpdir(), 'pgo-workload-'));
  const source = dirname(env.WORKERD_CONFIG);
  for (const file of readdirSync(source).filter((f) => f.endsWith('.js'))) {
    copyFileSync(join(source, file), join(dir, file));
  }
  const config = readFileSync(env.WORKERD_CONFIG, 'utf8');
  writeFileSync(
    join(dir, 'config.capnp'),
    config.replaceAll('COMPAT_DATE', newestCompatDate())
  );
  return join(dir, 'config.capnp');
}

// Runs `workerd serve` of the Workers, without the inspector or verbose logging, which production
// doesn't have either. stop() sends SIGTERM, which drains the server and lets it exit normally, so
// that an instrumented binary writes its profile.
async function serve() {
  const server = new WorkerdServerHarness({
    workerdBinary: env.WORKERD_BINARY,
    workerdConfig: writeConfig(),
    listenPortNames: SOCKETS,
    inspector: false,
    verbose: false,
  });
  await server.start();
  const ports = new Map();
  for (const socket of SOCKETS) {
    ports.set(socket, await server.getListenPort(socket));
  }
  return { ports, stop: () => server.stop() };
}

// Sends `count` requests, CONCURRENCY at a time, and checks each response.
async function load(count, url, init, check) {
  let sent = 0;
  const client = async () => {
    while (sent < count) {
      sent++;
      const response = await fetch(url, init);
      await check(response);
    }
  };
  await Promise.all(Array.from({ length: CONCURRENCY }, client));
}

const order = {
  customer: 'c-42',
  items: [
    { sku: 'a', price: 3.5, qty: 2 },
    { sku: 'b', price: 10, qty: 1 },
    { sku: 'c', price: 0.25, qty: 12 },
  ],
};
const upstreamText = 'hello from upstream '.repeat(32);
const longText = 'abc'.repeat(700);

// Each entry: the socket, the path, how many requests training sends relative to the others, the
// request, and a check of the response. The hello worlds weigh more: they are mostly the runtime's
// own code, which every Worker runs.
const WORKLOADS = [
  ['hello', '/', 4, {}, expectText(200, 'hello world')],
  ['hello-async', '/', 4, {}, expectText(200, 'hello world')],
  [
    'mixed',
    '/json',
    1,
    {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(order),
    },
    async (response) => {
      assert.strictEqual(response.status, 200);
      const body = await response.json();
      assert.deepStrictEqual(
        body.items.map((item) => item.total),
        [7, 10, 3]
      );
    },
  ],
  [
    'mixed',
    '/headers',
    1,
    {
      headers: {
        'accept-language': 'en-US,en;q=0.9',
        cookie: 'session=abc123; theme=dark',
        'user-agent': 'pgo-workload',
      },
    },
    async (response) => {
      assert.strictEqual(await response.text(), 'pgo-workload');
      assert.strictEqual(
        response.headers.get('x-echo-cookie'),
        'SESSION=ABC123; THEME=DARK'
      );
      assert.deepStrictEqual(response.headers.getSetCookie(), [
        'a=1; Path=/',
        'b=2; Path=/',
      ]);
    },
  ],
  [
    'mixed',
    '/stream',
    1,
    {},
    expectText(
      200,
      Array.from({ length: 8 }, (_, i) => `chunk ${i + 1}\n`).join('')
    ),
  ],
  ['mixed', '/transform', 1, {}, expectText(200, upstreamText.toUpperCase())],
  ['mixed', '/subrequest?n=7', 1, {}, expectText(200, 'got 7 from upstream')],
  [
    'mixed',
    '/crypto?key=value',
    1,
    {},
    async (response) => {
      assert.strictEqual(response.status, 200);
      assert.match(await response.text(), /^[0-9a-f]{64} [0-9a-f-]{36}$/);
    },
  ],
  [
    'mixed',
    '/url?a=1&b=two',
    1,
    {},
    expectText(
      200,
      'https://example.com/a/c?x=1&y=2#frag a=1&b=two&z=3 a%20b%26c'
    ),
  ],
  [
    'mixed',
    '/text',
    1,
    { method: 'POST', body: longText },
    expectText(201, longText.split('').reverse().join('')),
  ],
  ['mixed', '/missing', 1, {}, expectText(404, 'not found')],
];

function expectText(status, text) {
  return async (response) => {
    assert.strictEqual(response.status, status);
    assert.strictEqual(await response.text(), text);
  };
}

test('serving workload', async () => {
  const server = await serve();
  try {
    for (const [socket, path, weight, init, check] of WORKLOADS) {
      const url = `http://127.0.0.1:${server.ports.get(socket)}${path}`;
      const count = TRAINING ? weight * 10000 : 50;
      const start = performance.now();
      await load(count, url, init, check);
      const seconds = (performance.now() - start) / 1000;
      console.log(
        `${socket}${path}: ${count} requests in ${seconds.toFixed(1)}s`
      );
    }
  } finally {
    const [code, signal] = await server.stop();
    assert.ok(
      code === 0 || signal === 'SIGTERM',
      `code ${code}, signal ${signal}`
    );
  }
});
