// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import { execFileSync } from 'node:child_process';
import { globSync } from 'node:fs';
import http from 'node:http';
import { env } from 'node:process';
import { after, before, describe, test } from 'node:test';
import { WorkerdServerHarness } from '../server-harness.mjs';

// Both match config.capnp.
const SESSION_ID = 'container-session-test';
const UNIQUE_KEY = 'container-session-actor';

const SESSION_LABEL = 'dev.workerd.session';
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
            resolve(body === '' ? undefined : JSON.parse(body));
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

const sessionFilter = encodeURIComponent(
  JSON.stringify({ label: [`${SESSION_LABEL}=${SESSION_ID}`] })
);

async function listSessionContainers() {
  return dockerRequest(`/containers/json?all=true&filters=${sessionFilter}`);
}

async function listSessionVolumes() {
  const { Volumes } = await dockerRequest(`/volumes?filters=${sessionFilter}`);
  return Volumes ?? [];
}

// Removes what the session left behind, dependents first: applications join their sidecar's
// network namespace and mount the clone volumes.
async function removeSessionResources() {
  const containers = await listSessionContainers();
  const isSidecar = (container) =>
    container.Names.some((name) => name.endsWith('-proxy'));
  for (const container of containers.filter((c) => !isSidecar(c))) {
    await dockerRequest(`/containers/${container.Id}?force=true`, 'DELETE');
  }
  for (const volume of await listSessionVolumes()) {
    await dockerRequest(`/volumes/${volume.Name}`, 'DELETE');
  }
  for (const container of containers.filter(isSidecar)) {
    await dockerRequest(`/containers/${container.Id}?force=true`, 'DELETE');
  }
}

describe('Docker configuration with a session ID', () => {
  const snapshotVolumes = [];
  let workerd;
  let httpPort;

  before(async () => {
    // The images the containers run, as tarballs among the test's runfiles.
    for (const tarball of globSync('images/**/tarball.tar')) {
      execFileSync('docker', ['load', '--quiet', '--input', tarball], {
        stdio: 'inherit',
      });
    }
    await removeSessionResources();
    workerd = new WorkerdServerHarness({
      workerdBinary: env.WORKERD_BINARY,
      workerdConfig: env.WORKERD_CONFIG,
      listenPortNames: ['http'],
      extraArgs: ['--experimental'],
    });
    await workerd.start();
    httpPort = await workerd.getListenPort('http');
  });

  after(async () => {
    await workerd?.stop();
    await removeSessionResources();
    for (const name of snapshotVolumes) {
      await dockerRequest(`/volumes/${name}`, 'DELETE');
    }
  });

  test('names and labels the application container and sidecar', async () => {
    const response = await fetch(`http://127.0.0.1:${httpPort}/start`);
    assert.strictEqual(await response.text(), 'started');

    const names = (await listSessionContainers())
      .flatMap((container) => container.Names)
      .sort();
    assert.strictEqual(names.length, 2, JSON.stringify(names));
    const prefix = `/workerd-${SESSION_ID}-${UNIQUE_KEY}-`;
    assert.match(names[0], new RegExp(`^${prefix}[0-9a-f]{64}$`));
    assert.strictEqual(names[1], `${names[0]}-proxy`);
  });

  test('labels snapshot clone volumes but not snapshot volumes', async () => {
    const response = await fetch(`http://127.0.0.1:${httpPort}/restore`);
    assert.strictEqual(response.status, 200);
    const snapshot = await response.json();
    const snapshotVolume = `workerd-snap-${snapshot.id}`;
    snapshotVolumes.push(snapshotVolume);

    const cloneVolumes = (await listSessionVolumes()).map(({ Name }) => Name);
    assert.strictEqual(cloneVolumes.length, 1, JSON.stringify(cloneVolumes));
    assert.match(cloneVolumes[0], /^workerd-snap-clone-/);

    const { Labels } = await dockerRequest(`/volumes/${snapshotVolume}`);
    assert.strictEqual(Labels?.[SESSION_LABEL], undefined);
  });
});
