// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { env } from 'node:process';
import { beforeEach, afterEach, test } from 'node:test';
import { createSocket } from 'node:dgram';
import { scheduler } from 'node:timers/promises';
import assert from 'node:assert';
import { WorkerdServerHarness } from '../server-harness.mjs';

let workerd;

assert(
  env.WORKERD_BINARY !== undefined,
  'You must set the WORKERD_BINARY environment variable.'
);
assert(
  env.WORKERD_CONFIG !== undefined,
  'You must set the WORKERD_CONFIG environment variable.'
);

beforeEach(async () => {
  workerd = new WorkerdServerHarness({
    workerdBinary: env.WORKERD_BINARY,
    workerdConfig: env.WORKERD_CONFIG,
    listenPortNames: ['udp'],
    extraArgs: ['--experimental'],
  });
  await workerd.start();
  await workerd.getListenPort('udp');
});

afterEach(async () => {
  const [code, signal] = await workerd.stop();
  assert(code === 0 || signal === 'SIGTERM');
  workerd = null;
});

function sendAndReceive(client, port, data, timeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(
      () => reject(new Error('timed out waiting for reply datagram')),
      timeoutMs
    );
    client.once('message', (msg) => {
      clearTimeout(timeout);
      resolve(msg);
    });
    client.send(data, port, '127.0.0.1', (err) => {
      if (err) {
        clearTimeout(timeout);
        reject(err);
      }
    });
  });
}

function bound() {
  const client = createSocket('udp4');
  return new Promise((resolve) => {
    client.bind(0, '127.0.0.1', () => resolve(client));
  });
}

test('node:dgram echoes datagrams with the peer rinfo', async () => {
  const port = await workerd.getListenPort('udp');
  const client = await bound();
  try {
    const me = `127.0.0.1:${client.address().port}:IPv4`;
    const first = await sendAndReceive(client, port, Buffer.from('hello'));
    assert.strictEqual(first.toString(), `${me}:5:hello`);
    // The same peer stays on one flow; a later datagram is echoed the same way.
    const second = await sendAndReceive(client, port, Buffer.from('again!'));
    assert.strictEqual(second.toString(), `${me}:6:again!`);
    // After the idle timeout a new flow is created; the socket keeps serving.
    await scheduler.wait(300);
    const third = await sendAndReceive(client, port, Buffer.from('back'));
    assert.strictEqual(third.toString(), `${me}:4:back`);
  } finally {
    client.close();
  }
});

test('node:dgram serves several peers at once', async () => {
  const port = await workerd.getListenPort('udp');
  const clients = await Promise.all([bound(), bound(), bound()]);
  try {
    const replies = await Promise.all(
      clients.map((client, i) =>
        sendAndReceive(client, port, Buffer.from(`peer-${i}`))
      )
    );
    replies.forEach((reply, i) => {
      assert.strictEqual(
        reply.toString(),
        `127.0.0.1:${clients[i].address().port}:IPv4:6:peer-${i}`
      );
    });
  } finally {
    for (const client of clients) client.close();
  }
});
