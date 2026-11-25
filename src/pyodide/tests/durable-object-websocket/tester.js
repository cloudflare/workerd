// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
export default {
  async test(ctrl, env, ctx) {
    const resp = await env.PYTHON.fetch('http://example.com/websocket', {
      headers: { Upgrade: 'websocket' },
    });
    const ws = resp.webSocket;
    if (!ws) {
      throw new Error('No websocket');
    }
    ws.accept();
    const messagePromise = new Promise((resolve) => {
      ws.addEventListener('message', (msg) => {
        console.log('Received in JS Tester: ', msg.data);
        resolve(msg.data);
      });
    });
    const closePromise = new Promise((resolve) => {
      ws.addEventListener('close', resolve);
    });
    ws.send('test');

    await messagePromise;
    // The Durable Object replies to the close, so the handshake finishes now rather than when the
    // idle Durable Object is evicted.
    ws.close(1000, 'done');
    await closePromise;
  },
};
