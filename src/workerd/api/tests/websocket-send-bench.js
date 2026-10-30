// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A server that sends to every connected WebSocket once per frame: each call sends one message on
// each of `count` accepted WebSockets and waits until all of them arrive at the other end. Every
// send finds its socket's queue drained, so it starts a new outgoing pump.

const COUNTS = [1, 16, 64, 256];

function openPairs(count) {
  const servers = [];
  const delivery = { pending: 0, resolve: null };
  for (let i = 0; i < count; i++) {
    const [client, server] = Object.values(new WebSocketPair());
    client.accept();
    server.accept();
    client.addEventListener('message', () => {
      if (--delivery.pending === 0) delivery.resolve();
    });
    servers.push(server);
  }
  return { servers, delivery };
}

export default {
  bench(b) {
    const message = new Uint8Array(256);
    for (const count of COUNTS) {
      const { servers, delivery } = openPairs(count);
      b.run(`send to ${count} sockets`, () => {
        const delivered = new Promise((resolve) => {
          delivery.resolve = resolve;
        });
        delivery.pending = count;
        for (const server of servers) {
          server.send(message);
        }
        return delivered;
      });
    }
  },
};
