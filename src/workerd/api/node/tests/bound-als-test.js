import { AsyncLocalStorage } from 'node:async_hooks';
import { strictEqual } from 'node:assert';

// This test verifies that the AsyncLocalStorage snapshot and bind APIs appropriately
// throw an error when called from outside the request context in which they were created.
// This is a workers specific limitation and is not present in Node.js.

// The test will only work if the associated compatibility flag is enabled.
strictEqual(
  Cloudflare.compatibilityFlags.bind_asynclocalstorage_snapshot_to_request,
  true
);

const kErr =
  'Cannot call this AsyncLocalStorage bound function outside of the ' +
  'request in which it was created.';

export const test = {
  async test(_, env) {
    const resp = await env.subrequest.fetch(
      'http://AsyncLocalStorage/snapshot'
    );
    strictEqual(resp.status, 200);
    strictEqual(await resp.text(), 'ok');
    const resp2 = await env.subrequest.fetch(
      'http://AsyncLocalStorage/snapshot'
    );
    strictEqual(resp2.status, 500);
    strictEqual(await resp2.text(), kErr);

    const resp3 = await env.subrequest.fetch('http://AsyncLocalStorage/bind');
    strictEqual(resp3.status, 200);
    strictEqual(await resp3.text(), 'ok');
    const resp4 = await env.subrequest.fetch('http://AsyncLocalStorage/bind');
    strictEqual(resp4.status, 500);
    strictEqual(await resp4.text(), kErr);
  },
};

let snapshot;
let boundFn;

const als = new AsyncLocalStorage();
const noRequestSnapshot = als.run(123, () => AsyncLocalStorage.snapshot());

export default {
  async fetch(req) {
    // A snapshot created outside of a request can still be used, and can
    // perform i/o operation...
    strictEqual(
      noRequestSnapshot(() => {
        // Works if no error is thrown. We don't care about the actual
        // callback here.
        setTimeout(() => {}, 0);
        return als.getStore();
      }),
      123
    );

    try {
      if (req.url.endsWith('/snapshot')) {
        snapshot ??= AsyncLocalStorage.snapshot();
        snapshot(() => {});
        return new Response('ok');
      } else if (req.url.endsWith('/bind')) {
        boundFn ??= AsyncLocalStorage.bind(() => {});
        boundFn();
        return new Response('ok');
      }
      throw new Error('Unknown request');
    } catch (err) {
      return new Response(err.message, { status: 500 });
    }
  },
};
