// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, match, ok, strictEqual } from 'node:assert';

async function check(kind, env) {
  for (const service of [env.current, env.legacy]) {
    const retained = await (
      await service.fetch('https://fixture/retained?kind=' + kind)
    ).json();
    ok(retained.events.includes(kind === '204' ? 'error' : 'open'));
    strictEqual(retained.readyState, EventSource.CLOSED);

    // Finishing this response destroys request A before request B resumes its callback.
    strictEqual(
      await (await service.fetch('https://fixture/create')).text(),
      'created'
    );
    const resumed = await (
      await service.fetch('https://fixture/resume?kind=' + kind)
    ).json();
    deepStrictEqual(resumed.events, []);
    strictEqual(resumed.readyState, EventSource.CONNECTING);
    match(resumed.closeError, /different request/);
  }
}

export const delayedNoBody = {
  async test(_controller, env) {
    await check('204', env);
  },
};

export const delayedBody = {
  async test(_controller, env) {
    await check('200', env);
  },
};

export const closeDuringReconnect = {
  async test(_controller, env) {
    const source = new EventSource('https://fixture/empty', {
      fetcher: env.current,
    });
    const { promise, resolve } = Promise.withResolvers();
    source.onerror = () => {
      source.close();
      source.close();
      resolve();
    };
    await promise;
    await scheduler.wait(0);
    strictEqual(source.readyState, EventSource.CLOSED);
  },
};

export const sameContextMessageDelivery = {
  async test() {
    const stream = new IdentityTransformStream();
    const source = EventSource.from(stream.readable);
    const { promise, resolve } = Promise.withResolvers();
    const messages = [];
    source.onmessage = (event) => {
      messages.push([event.data, event.lastEventId]);
      if (messages.length === 2) resolve();
    };
    const writer = stream.writable.getWriter();
    const write = writer.write(
      new TextEncoder().encode(
        'id: first\ndata: one\n\nid: second\ndata: two\n\n'
      )
    );
    const close = writer.close();
    await promise;
    deepStrictEqual(messages, [
      ['one', 'first'],
      ['two', 'second'],
    ]);
    await Promise.all([write, close]);
    source.close();
    source.close();
    strictEqual(source.readyState, EventSource.CLOSED);
  },
};
