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

// Sends `data` to the given port and waits for exactly one reply datagram, rejecting if none
// arrives within the timeout.
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

test('UDP connect() drops datagrams once the flow exceeds maxPendingBytes', async () => {
  // The configured maxPendingBytes is 3. The worker echoes a priming ping immediately, which
  // confirms it's already running (isolate/module startup already paid for) and about to enter
  // its 300ms delay -- only then do we fire the burst of single-byte datagrams that's meant to
  // overflow the queue. Only the first 3 bytes fit; the rest are dropped rather than buffered.
  const port = await workerd.getListenPort('udp');
  const client = createSocket('udp4');
  try {
    // The priming ping must itself fit within maxPendingBytes (3), hence one byte, not the
    // word "ping".
    await sendAndReceive(client, port, Buffer.from('p'));

    const received = [];
    client.on('message', (msg) => received.push(msg.toString()));

    for (const byte of ['0', '1', '2', '3', '4', '5']) {
      client.send(Buffer.from(byte), port, '127.0.0.1');
    }

    // Give the worker time to wake up from its delay and echo back whatever fit in the queue.
    await scheduler.wait(1000);

    assert.deepStrictEqual(received, ['0', '1', '2']);
  } finally {
    client.close();
  }
});
