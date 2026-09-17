// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import http from 'node:http';
import { env } from 'node:process';
import { test } from 'node:test';
import { WorkerdServerHarness } from '../server-harness.mjs';

const CONTAINER_NAME_FRAGMENT = 'workerd-container-shutdown-test-';
const DOCKER_SOCKET_PATH = '/var/run/docker.sock';

assert.notStrictEqual(
  env.WORKERD_BINARY,
  undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert.notStrictEqual(
  env.WORKERD_CONFIG,
  undefined,
  'You must set the WORKERD_CONFIG environment variable.'
);

function dockerRequest(path, method = 'GET') {
  return new Promise((resolve, reject) => {
    const request = http.request(
      { method, path, socketPath: DOCKER_SOCKET_PATH },
      (response) => {
        const chunks = [];
        response.on('data', (chunk) => chunks.push(chunk));
        response.once('end', () => {
          const body = Buffer.concat(chunks).toString();
          if (response.statusCode >= 200 && response.statusCode < 300) {
            resolve(body);
          } else {
            reject(
              new Error(
                `Docker ${method} ${path} failed with ${response.statusCode}: ${body}`
              )
            );
          }
        });
      }
    );
    request.once('error', reject);
    request.end();
  });
}

async function listTestContainers() {
  const filters = encodeURIComponent(
    JSON.stringify({ name: [CONTAINER_NAME_FRAGMENT] })
  );
  const body = await dockerRequest(
    `/containers/json?all=true&filters=${filters}`
  );
  return JSON.parse(body);
}

async function removeTestContainers() {
  const containers = await listTestContainers();
  containers.sort((a, b) => {
    const aIsSidecar = a.Names.some((name) => name.endsWith('-proxy'));
    const bIsSidecar = b.Names.some((name) => name.endsWith('-proxy'));
    return Number(aIsSidecar) - Number(bIsSidecar);
  });
  for (const container of containers) {
    await dockerRequest(`/containers/${container.Id}?force=true`, 'DELETE');
  }
}

test('graceful shutdown removes application and sidecar containers', async () => {
  await removeTestContainers();

  const workerd = new WorkerdServerHarness({
    workerdBinary: env.WORKERD_BINARY,
    workerdConfig: env.WORKERD_CONFIG,
    listenPortNames: ['http'],
  });
  let started = false;
  let stopped = false;

  try {
    await workerd.start();
    started = true;

    const httpPort = await workerd.getListenPort('http');
    const response = await fetch(`http://127.0.0.1:${httpPort}`);
    assert.strictEqual(response.status, 200);
    assert.strictEqual(await response.text(), 'started');

    const runningContainers = await listTestContainers();
    const names = runningContainers.flatMap((container) => container.Names);
    assert.strictEqual(runningContainers.length, 2, JSON.stringify(names));
    assert(
      names.some((name) => name.endsWith('-proxy')),
      JSON.stringify(names)
    );
    assert(
      names.some((name) => !name.endsWith('-proxy')),
      JSON.stringify(names)
    );

    const [code, signal] = await workerd.stop();
    stopped = true;
    assert.strictEqual(code, 0, `code=${code}, signal=${signal}`);
    assert.strictEqual(signal, null, `code=${code}, signal=${signal}`);

    assert.deepStrictEqual(await listTestContainers(), []);
  } finally {
    if (started && !stopped) {
      await workerd.stop();
    }
    await removeTestContainers();
  }
});
