// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

// The pump lowers bufferedAmount after each write completes, in a continuation that can run
// after the receiving end has already dispatched the message, so tests wait for it here.
async function waitUntilDrained(ws) {
  while (ws.bufferedAmount !== 0) {
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
}

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

export const startsAtZero = {
  async test() {
    const [client, server] = Object.values(new WebSocketPair());
    strictEqual(client.bufferedAmount, 0);
    strictEqual(server.bufferedAmount, 0);
    client.accept();
    server.accept();
    strictEqual(server.bufferedAmount, 0);
    await closeBoth(client, server);
  },
};

// Text counts as its UTF-8 length, binary data as its byte length.
export const countsPayloadBytes = {
  async test() {
    const [client, server] = Object.values(new WebSocketPair());
    client.accept();
    server.accept();
    const received = receiveMessages(client, 3);

    server.send('h€llo'); // The euro sign takes 3 bytes in UTF-8.
    strictEqual(server.bufferedAmount, 7);
    server.send(new Uint8Array(1000));
    strictEqual(server.bufferedAmount, 1007);
    server.send(new ArrayBuffer(24));
    strictEqual(server.bufferedAmount, 1031);

    await received;
    await waitUntilDrained(server);
    await closeBoth(client, server);
  },
};

// Messages stay counted for as long as the peer isn't reading them.
export const staysCountedUntilPeerReads = {
  async test() {
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();

    const chunk = new Uint8Array(64 * 1024);
    for (let i = 0; i < 4; i++) server.send(chunk);
    strictEqual(server.bufferedAmount, 4 * chunk.byteLength);

    // Give the pump the chance to write. It can't: nothing reads the other end of the pair.
    for (let i = 0; i < 10; i++) {
      await new Promise((resolve) => setTimeout(resolve, 0));
    }
    strictEqual(server.bufferedAmount, 4 * chunk.byteLength);

    const received = receiveMessages(client, 4);
    client.accept();
    await received;
    await waitUntilDrained(server);
    await closeBoth(client, server);
  },
};

// A Close frame's reason isn't application data, so close() doesn't change bufferedAmount.
export const closeIsNotCounted = {
  async test() {
    const [client, server] = Object.values(new WebSocketPair());
    client.accept();
    server.accept();
    const closed = Promise.all([whenClosed(client), whenClosed(server)]);

    server.send('0123456789');
    server.close(1000, 'a reason that is not application data');
    strictEqual(server.bufferedAmount, 10);

    await waitUntilDrained(server);
    client.close();
    await closed;
    strictEqual(server.bufferedAmount, 0);
  },
};
