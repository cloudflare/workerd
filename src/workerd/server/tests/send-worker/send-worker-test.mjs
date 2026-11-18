// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { env } from 'node:process';
import { before, after, test } from 'node:test';
import assert from 'node:assert';
import { WorkerdServerHarness } from '../server-harness.mjs';

assert(
  env.WORKERD_BINARY !== undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert(
  env.WORKERD_CONFIG !== undefined,
  'You must set the WORKERD_CONFIG environment variable.'
);

// Every socket of the config that serves the whole toy: hello, `/bytes/<n>` and the count of an
// upload. Each reaches the same toy by a different path.
const TOYS = [
  // On the KJ loop.
  'kj-toy',
  // A send worker behind the adapter.
  'send-toy',
  // A send worker that forwards to `kj-toy` as a subrequest.
  'send-fetch',
  'send-fetch-copy',
  // JavaScript Workers whose service binding is the toy, a send worker, or a send worker that
  // forwards: the adapter called from C++, and a send worker's subrequest in a chain.
  'js-fetch',
  'js-fetch-send-toy',
  'js-fetch-send-fetch',
];

const HELLOS = [...TOYS, 'send-hello', 'js-hello'];

let workerd;
const ports = new Map();

before(async () => {
  workerd = new WorkerdServerHarness({
    workerdBinary: env.WORKERD_BINARY,
    workerdConfig: env.WORKERD_CONFIG,
    listenPortNames: HELLOS,
  });
  await workerd.start();
  for (const name of HELLOS) {
    ports.set(name, await workerd.getListenPort(name));
  }
});

after(async () => {
  const [code, signal] = await workerd.stop();
  assert(code === 0 || signal === 'SIGTERM');
});

function url(name, path = '/') {
  return `http://127.0.0.1:${ports.get(name)}${path}`;
}

for (const name of HELLOS) {
  test(`${name} says hello`, async () => {
    const response = await fetch(url(name));
    assert.strictEqual(response.status, 200);
    assert.strictEqual(response.headers.get('content-type'), 'text/plain');
    assert.strictEqual(response.headers.get('content-length'), '13');
    assert.strictEqual(await response.text(), 'Hello, world!');
  });
}

for (const name of TOYS) {
  test(`${name} streams a large response`, async () => {
    const size = 32 * 1024 * 1024 + 123;
    const response = await fetch(url(name, `/bytes/${size}`));
    assert.strictEqual(response.status, 200);
    assert.strictEqual(response.headers.get('content-length'), `${size}`);
    let received = 0;
    for await (const chunk of response.body) {
      received += chunk.length;
    }
    assert.strictEqual(received, size);
  });

  test(`${name} reads a large request body of known length`, async () => {
    const body = new Uint8Array(16 * 1024 * 1024 + 7);
    const response = await fetch(url(name), { method: 'POST', body });
    assert.strictEqual(response.status, 200);
    assert.strictEqual(await response.text(), `${body.length}`);
  });

  test(`${name} reads a chunked request body`, async () => {
    const chunk = new Uint8Array(100_000);
    const chunks = 50;
    const body = new ReadableStream({
      start(controller) {
        for (let i = 0; i < chunks; i++) {
          controller.enqueue(chunk);
        }
        controller.close();
      },
    });
    const response = await fetch(url(name), {
      method: 'POST',
      body,
      duplex: 'half',
    });
    assert.strictEqual(response.status, 200);
    assert.strictEqual(await response.text(), `${chunk.length * chunks}`);
  });

  test(`${name} serves concurrent requests`, async () => {
    const responses = await Promise.all(
      Array.from({ length: 64 }, (_, i) =>
        fetch(url(name, `/bytes/${1000 + i}`)).then((r) => r.arrayBuffer())
      )
    );
    responses.forEach((body, i) =>
      assert.strictEqual(body.byteLength, 1000 + i)
    );
  });
}

test('a client that goes away mid-response leaves the server serving', async () => {
  for (const name of ['send-toy', 'send-fetch', 'send-fetch-copy']) {
    const controller = new AbortController();
    const response = await fetch(url(name, `/bytes/${1024 * 1024 * 1024}`), {
      signal: controller.signal,
    });
    const reader = response.body.getReader();
    await reader.read();
    controller.abort();
    await assert.rejects(reader.read());
    const after = await fetch(url(name));
    assert.strictEqual(await after.text(), 'Hello, world!');
  }
});
