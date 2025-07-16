import { registerFetchEvents } from 'cloudflare:workers';
import { mock } from 'node:test';
import { strictEqual } from 'node:assert';

export const testListener = {
  async test(_ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    const listener = mock.fn((event) => {
      const response = new Response('Hello, World!');
      const responseEvent = new CustomEvent(`nodejs.fetch-${event.clientId}`, {
        detail: response,
      });
      dispatchEvent(responseEvent);
      resolve();
    });
    addEventListener('nodejs.fetch', listener);

    const res = await env.SERVICE.fetch('http://example.com');
    strictEqual(res.status, 200);
    strictEqual(await res.text(), 'Hello, World!');
    await promise;
    strictEqual(listener.mock.callCount(), 1);
  },
};

export default registerFetchEvents();
