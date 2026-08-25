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

test('UDP connect() handler reports protocol "udp"', async () => {
  const port = await workerd.getListenPort('udp');
  const client = createSocket('udp4');
  try {
    const reply = await sendAndReceive(client, port, Buffer.from('hello'));
    assert.strictEqual(reply.toString(), 'first:udp:hello');
  } finally {
    client.close();
  }
});

test('UDP connect() preserves datagram boundaries for a >4KiB datagram', async () => {
  // Byte-mode streams would split this at the auto-allocated chunk size (4KiB/16KiB); the
  // value-mode datagram readable must deliver it as a single chunk.
  const port = await workerd.getListenPort('udp');
  const client = createSocket('udp4');
  try {
    const first = await sendAndReceive(client, port, Buffer.from('start'));
    assert.strictEqual(first.toString(), 'first:udp:start');

    const big = Buffer.alloc(20000, 'X');
    const reply = await sendAndReceive(client, port, big);
    assert.strictEqual(reply.toString(), 'echo:' + big.toString());
  } finally {
    client.close();
  }
});

test('UDP connect() groups datagrams from one peer into a single flow', async () => {
  const port = await workerd.getListenPort('udp');
  const client = createSocket('udp4');
  try {
    const first = await sendAndReceive(client, port, Buffer.from('a'));
    assert.strictEqual(first.toString(), 'first:udp:a');
    // Since this is the same client socket (same source port), it's the same peer address, so
    // this should land on the same flow / connect() call rather than starting a new one.
    const second = await sendAndReceive(client, port, Buffer.from('b'));
    assert.strictEqual(second.toString(), 'echo:b');
  } finally {
    client.close();
  }
});

test('UDP connect() starts a new flow after the idle timeout', async () => {
  const port = await workerd.getListenPort('udp');
  const client = createSocket('udp4');
  try {
    const before = await sendAndReceive(client, port, Buffer.from('x'));
    assert.strictEqual(before.toString(), 'first:udp:x');

    // The configured idleTimeoutMs is 1000; wait well past it.
    await scheduler.wait(1500);

    const after = await sendAndReceive(client, port, Buffer.from('y'));
    // A new flow means a new connect() call, so this is a "first" reply again rather than "echo".
    assert.strictEqual(after.toString(), 'first:udp:y');
  } finally {
    client.close();
  }
});
