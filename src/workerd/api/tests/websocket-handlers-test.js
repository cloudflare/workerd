// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, strictEqual } from 'node:assert';

const kTypes = ['open', 'message', 'close', 'error'];

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

function acceptedPair() {
  const [client, server] = Object.values(new WebSocketPair());
  client.accept();
  server.accept();
  return [client, server];
}

export const handlerDefaults = {
  test() {
    const [ws] = Object.values(new WebSocketPair());
    for (const type of kTypes) {
      const name = `on${type}`;
      strictEqual(ws[name], null, name);
      strictEqual(name in ws, true, name);
      strictEqual(Object.hasOwn(ws, name), false, name);
      strictEqual(
        typeof Object.getOwnPropertyDescriptor(WebSocket.prototype, name).set,
        'function',
        name
      );
    }
  },
};

export const nonObjectBecomesNull = {
  test() {
    const [ws] = Object.values(new WebSocketPair());
    for (const type of kTypes) {
      const name = `on${type}`;
      for (const value of [2, 'string', true, Symbol(), 1n, null, undefined]) {
        const fn = () => {};
        ws[name] = fn;
        strictEqual(ws[name], fn, name);
        ws[name] = value;
        strictEqual(ws[name], null, `${name} = ${String(value)}`);
        strictEqual(Object.hasOwn(ws, name), false, name);
      }
    }
  },
};

export const nonCallableObjectRetained = {
  async test() {
    const [client, server] = acceptedPair();
    let called = false;
    const object = {
      handleEvent() {
        called = true;
      },
    };
    server.onmessage = object;
    strictEqual(server.onmessage, object);

    const { promise, resolve } = Promise.withResolvers();
    server.addEventListener('message', () => resolve());
    client.send('hi');
    await promise;
    strictEqual(called, false);
    strictEqual(server.onmessage, object);
    await closeBoth(client, server);
  },
};

export const handlerPosition = {
  async test() {
    const [client, server] = acceptedPair();
    const order = [];
    const { promise, resolve } = Promise.withResolvers();
    server.addEventListener('message', () => {
      order.push('a');
    });
    server.onmessage = () => {
      order.push('wrong');
    };
    server.addEventListener('message', () => {
      order.push('c');
      resolve();
    });
    // Reassigning keeps the handler's position in the listener list.
    server.onmessage = (event) => {
      order.push(`b:${event.data}`);
    };
    client.send('hi');
    await promise;
    deepStrictEqual(order, ['a', 'b:hi', 'c']);
    await closeBoth(client, server);
  },
};

export const closeHandler = {
  async test() {
    const [client, server] = acceptedPair();
    const { promise, resolve } = Promise.withResolvers();
    let calls = 0;
    server.onclose = (event) => {
      calls++;
      resolve(event.code);
    };
    const clientClosed = whenClosed(client);
    client.close(1000, 'done');
    strictEqual(await promise, 1000);
    server.close();
    await clientClosed;
    strictEqual(calls, 1);
  },
};
