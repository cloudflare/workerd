// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A simple test to confirm we can close() a websocket from the close handler.
export class DurableObjectExample {
  constructor(state) {
    this.reset = true;
    this.state = state;
  }

  async fetch(request) {
    // Confirm this is a websocket request.
    const upgradeHeader = request.headers.get('Upgrade');
    if (!upgradeHeader || upgradeHeader !== 'websocket') {
      return new Response('Expected Upgrade: websocket', { status: 426 });
    }

    let pair = new WebSocketPair();
    let server = pair[0];
    if (request.url.endsWith('/hibernation')) {
      this.state.acceptWebSocket(server);
    } else {
      server.accept();
      server.addEventListener('message', () => {
        server.send('regular message from DO');
      });
      server.addEventListener('close', () => {
        server.close(1000, 'regular close from DO');
      });
    }

    return new Response(null, {
      status: 101,
      webSocket: pair[1],
    });
  }

  webSocketMessage(ws) {
    ws.send(`Hibernatable message from DO.`);
  }

  webSocketClose(ws, code, reason, wasClean) {
    ws.close(1000, 'Hibernatable close from DO');
  }
}

export default {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName('foo');
    let obj = env.ns.get(id);

    // Test to make sure that we can call ws.close() from the DO's close handler.
    let webSocketTest = async (obj, url, expected) => {
      let req = await obj.fetch(url, {
        headers: {
          Upgrade: 'websocket',
        },
      });

      let ws = req.webSocket;
      if (!ws) {
        return new Error('Failed to get ws');
      }
      ws.accept();
      let prom = new Promise((resolve, reject) => {
        ws.addEventListener('close', (close) => {
          if ((close.code != 1000) & (close.reason != expected)) {
            reject(`got ${close.reason}`);
          }
          resolve();
        });
      });

      ws.send('Hi from Worker!');
      ws.close(1000, 'bye from Worker!');

      await prom;
    };

    // Normal websocket
    await webSocketTest(obj, 'http://example.com/', 'regular close from DO');
    // Hibernatable Websocket.
    await webSocketTest(
      obj,
      'http://example.com/hibernation',
      'Hibernatable close from DO'
    );
  },
};
