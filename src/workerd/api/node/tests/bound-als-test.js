// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { AsyncLocalStorage, AsyncResource } from 'node:async_hooks';
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

// The functions the bind and snapshot APIs return are made without consulting
// Function.prototype.bind, which a Worker is free to replace or delete.
export const independentOfFunctionPrototypeBind = {
  test() {
    const descriptor = Object.getOwnPropertyDescriptor(
      Function.prototype,
      'bind'
    );
    const originalBind = descriptor.value;
    let replacedBindCalls = 0;
    let bound, snapshot, boundToResource, boundWithoutBind;
    const storage = new AsyncLocalStorage();
    const receiver = {};
    try {
      Function.prototype.bind = function (...args) {
        replacedBindCalls++;
        return Reflect.apply(originalBind, this, args);
      };
      storage.run('store', () => {
        bound = AsyncLocalStorage.bind((a, b) =>
          [storage.getStore(), a, b].join()
        );
        snapshot = AsyncLocalStorage.snapshot();
        boundToResource = new AsyncResource('test').bind(function (a) {
          return [this === receiver, storage.getStore(), a].join();
        }, receiver);
      });
      strictEqual(replacedBindCalls, 0);

      delete Function.prototype.bind;
      boundWithoutBind = AsyncLocalStorage.bind(() => 'no bind');
    } finally {
      Object.defineProperty(Function.prototype, 'bind', descriptor);
    }

    strictEqual(bound(1, 2), 'store,1,2');
    strictEqual(
      snapshot((a) => [storage.getStore(), a].join(), 3),
      'store,3'
    );
    strictEqual(boundToResource(4), 'true,store,4');
    strictEqual(boundWithoutBind(), 'no bind');

    // As when they were functions made directly over a native callback.
    strictEqual(bound.name, '');
    strictEqual(bound.length, 0);
    strictEqual(snapshot.name, '');
  },
};
