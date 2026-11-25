// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, ok, strictEqual } from 'node:assert';

function whenClosed(ws) {
  return new Promise((resolve) => {
    ws.addEventListener('close', resolve);
  });
}

// An accepted end that is never closed keeps waiting for messages, and the test then fails as a
// hung request.
async function closeBoth(client, server) {
  const closed = Promise.all([whenClosed(client), whenClosed(server)]);
  client.close();
  server.close();
  await closed;
}

function receiveMessages(ws, count) {
  const messages = [];
  return new Promise((resolve) => {
    ws.addEventListener('message', (event) => {
      messages.push(event.data);
      if (messages.length === count) resolve(messages);
    });
  });
}

function acceptedPair() {
  const [client, server] = Object.values(new WebSocketPair());
  client.binaryType = 'arraybuffer';
  client.accept();
  server.accept();
  return [client, server];
}

function bytesOf(data) {
  ok(
    data instanceof ArrayBuffer,
    `expected a binary message, got ${typeof data}`
  );
  return [...new Uint8Array(data)];
}

function textOf(data) {
  ok(
    data instanceof ArrayBuffer,
    `expected a binary message, got ${typeof data}`
  );
  return new TextDecoder().decode(data);
}

export const sendsBlobAsBinary = {
  async test() {
    const [client, server] = acceptedPair();
    const received = receiveMessages(client, 1);
    server.send(new Blob([new Uint8Array([0, 1, 254, 255]), 'hi']));
    const [data] = await received;
    deepStrictEqual(bytesOf(data), [0, 1, 254, 255, 0x68, 0x69]);
    await closeBoth(client, server);
  },
};

export const sendsEmptyBlobAndFile = {
  async test() {
    const [client, server] = acceptedPair();
    const received = receiveMessages(client, 3);
    server.send(new Blob([]));
    server.send(new File(['file'], 'a.txt', { type: 'text/plain' }));
    server.send(new Blob(['abcdef']).slice(2, 4));
    const [empty, file, sliced] = await received;
    deepStrictEqual(bytesOf(empty), []);
    strictEqual(textOf(file), 'file');
    strictEqual(textOf(sliced), 'cd');
    await closeBoth(client, server);
  },
};

// A Blob is sent in the order send() was called, between the messages around it.
export const keepsOrderWithOtherSends = {
  async test() {
    const [client, server] = acceptedPair();
    const received = receiveMessages(client, 5);
    server.send('first');
    server.send(new Blob(['second']));
    server.send(new TextEncoder().encode('third'));
    server.send(new Blob(['fourth']));
    server.send('fifth');
    const messages = await received;
    deepStrictEqual(
      messages.map((m) => (typeof m === 'string' ? m : `binary:${textOf(m)}`)),
      ['first', 'binary:second', 'binary:third', 'binary:fourth', 'fifth']
    );
    await closeBoth(client, server);
  },
};

export const countsBlobInBufferedAmount = {
  async test() {
    const [client, server] = acceptedPair();
    const received = receiveMessages(client, 1);
    server.send(new Blob([new Uint8Array(10)]));
    strictEqual(server.bufferedAmount, 10);
    await received;
    await closeBoth(client, server);
  },
};
