// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { AsyncLocalStorage, AsyncResource } from 'node:async_hooks';
import { deepStrictEqual, strictEqual } from 'node:assert';

const als = new AsyncLocalStorage();
const kErr =
  'Cannot call this AsyncLocalStorage bound function outside of the ' +
  'request in which it was created.';
let resource;
let boundInRequestA;
let globalBoundInRequestA;
let rootScopedResource;
let rootScopedBoundInRequestA;
const globalResource = new AsyncResource('global');

export const test = {
  async test(_, env) {
    const sameRequest = await env.subrequest.fetch('http://test/same-request');
    strictEqual(sameRequest.status, 200);
    strictEqual(await sameRequest.text(), 'same-request');

    const create = await env.subrequest.fetch('http://test/create');
    strictEqual(create.status, 200);

    const results = [];
    for (const path of [
      'run',
      'call-bound',
      'bind-retained',
      'run-root-scoped',
      'call-root-scoped-bound',
      'bind-global',
      'call-global',
    ]) {
      const response = await env.subrequest.fetch(`http://test/${path}`);
      results.push([response.status, await response.text()]);
    }
    deepStrictEqual(results, [
      [500, kErr],
      [500, kErr],
      [500, kErr],
      [200, 'root'],
      [200, 'root'],
      [200, 'root'],
      [200, 'root,root'],
    ]);
  },
};

export default {
  fetch(request) {
    const path = new URL(request.url).pathname;
    if (path === '/same-request') {
      const receiver = {};
      return als.run('same-request', () => {
        const localResource = new AsyncResource('same-request');
        const result = localResource.runInAsyncScope(
          function (suffix) {
            strictEqual(this, receiver);
            return als.getStore() + suffix;
          },
          receiver,
          ''
        );
        return new Response(result);
      });
    }

    if (path === '/create') {
      als.run('request-a-secret', () => {
        resource = new AsyncResource('request-a');
        boundInRequestA = resource.bind(() => als.getStore());
      });
      globalResource.runInAsyncScope(() => {
        rootScopedResource = new AsyncResource('request-a-root-scope');
        rootScopedBoundInRequestA = rootScopedResource.bind(
          () => als.getStore() ?? 'root'
        );
      });
      return new Response('created');
    }

    try {
      let value;
      if (path === '/run') {
        value = als.run('request-b', () =>
          resource.runInAsyncScope(() => als.getStore())
        );
      } else if (path === '/call-bound') {
        value = boundInRequestA();
      } else if (path === '/bind-retained') {
        value = resource.bind(() => als.getStore())();
      } else if (path === '/run-root-scoped') {
        value = rootScopedResource.runInAsyncScope(
          () => als.getStore() ?? 'root'
        );
      } else if (path === '/call-root-scoped-bound') {
        value = rootScopedBoundInRequestA();
      } else if (path === '/bind-global') {
        globalBoundInRequestA = globalResource.bind(
          () => als.getStore() ?? 'root'
        );
        value = globalResource.runInAsyncScope(() => als.getStore() ?? 'root');
      } else if (path === '/call-global') {
        value = [
          globalBoundInRequestA(),
          globalResource.runInAsyncScope(() => als.getStore() ?? 'root'),
        ].join(',');
      } else {
        throw new Error('Unknown request');
      }
      return new Response(value);
    } catch (error) {
      return new Response(error.message, { status: 500 });
    }
  },
};
