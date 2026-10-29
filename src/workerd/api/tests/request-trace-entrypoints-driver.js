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
    const started = new Promise((resolve) =>
      socket.addEventListener('message', resolve, { once: true })
    );
    socket.send('first');
    assert.strictEqual((await started).data, 'first-started');
    const completed = new Promise((resolve) => {
      const messages = [];
      const listener = ({ data }) => {
        messages.push(data);
        if (messages.length === 2) {
          socket.removeEventListener('message', listener);
          resolve(messages);
        }
      };
      socket.addEventListener('message', listener);
    });
    socket.send('second');
    assert.deepStrictEqual(await completed, ['first-done', 'second-done']);
    const closed = new Promise((resolve) =>
      socket.addEventListener('close', resolve, { once: true })
    );
    socket.close(1000, 'done');
    await closed;
  },
};

export const restoreEntrypoints = {
  async test(_controller, env) {
    for (const kind of ['service', 'rpc']) {
      const response = await env.ACTOR.fetch(`http://actor/restore/${kind}`);
      assert.strictEqual(await response.text(), 'restored');
    }
  },
};
