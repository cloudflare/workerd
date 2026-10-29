// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

export const entrypoints = {
  async test(_controller, env) {
    const response = await env.ACTOR.fetch('http://actor/', {
      headers: { Upgrade: 'websocket' },
    });
    const socket = response.webSocket;
    socket.accept();
    const message = new Promise((resolve) =>
      socket.addEventListener('message', resolve, { once: true })
    );
    socket.send('check');
    assert.strictEqual((await message).data, 'ok');
    const closed = new Promise((resolve) =>
      socket.addEventListener('close', resolve, { once: true })
    );
    socket.close(1000, 'done');
    await closed;
  },
};
